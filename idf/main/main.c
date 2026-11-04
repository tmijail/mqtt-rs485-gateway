/*
 * RS485 (Modbus RTU master) -> MQTT Sparkplug B 3.0 gateway for the Waveshare
 * ESP32-S3-POE-ETH-8DI-8DO. app_main loads the configuration and wires the
 * components together; all work happens in their tasks:
 *
 *   rs485_bus   (core 1, prio 10)  gw_bus:  sole owner of the RS485 port, polls and writes
 *   sparkplug   (core 0, prio 8)   gw_edge: MQTT session, Sparkplug engine, NCMD/DCMD
 *   esp_timer                      gw_io:   1 kHz DI sampling and debounce
 *   status_led  (prio 2)           gw_status
 *   console     (prio 2)           gw_shell: diagnostic REPL on the console UART
 *   syslog      (prio 2)           gw_log:   copies the log to a syslog server (if configured)
 *   httpd       (prio 3)           gw_web:   REST API (and web UI) for the configuration
 *   + IDF tasks: esp-mqtt, lwIP tcpip, esp_eth, Wi-Fi (if configured), event loop
 */
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gw_ap.h"
#include "gw_bootbtn.h"
#include "gw_bsp.h"
#include "gw_bus.h"
#include "gw_config.h"
#include "gw_edge.h"
#include "gw_io.h"
#include "gw_log.h"
#include "gw_net.h"
#include "gw_port.h"
#include "gw_rs485.h"
#include "gw_shell.h"
#include "gw_status.h"
#include "gw_storage.h"
#include "gw_tagdb.h"
#include "gw_web.h"

static const char *TAG = "main";

static gw_config_t s_cfg;   /* immutable after boot; shared read-only by every task */
static char s_hardware[64];
static gw_node_info_t s_info;  /* NBIRTH "Node Info/..." */
static gw_cfg_err_t s_cfg_err;
static char s_commission_pw[GW_COMMISSION_PW_LEN + 1];
static gw_web_mode_t s_mode = GW_WEB_NORMAL;
static bool s_web_started;
static const char *s_source = "";
static volatile bool s_ready;   /* the mode is settled: BOOT gestures may act on it */

static void on_ip_changed(bool up)
{
    gw_edge_notify_ip(up);
    gw_log_notify_network(up);
}

static void on_device_updated(uint16_t dev)
{
    gw_edge_notify_device(dev);
}

static void restart_soon(void);

static void start_shell(const gw_config_t *cfg, const char *source)
{
    const gw_shell_ctx_t ctx = {.cfg = cfg,
                                .commissioning = s_mode == GW_WEB_COMMISSIONING,
                                .config_source = source,
                                .config_error = s_cfg_err.msg,
                                .info = &s_info,
                                .restart = cfg != NULL ? gw_edge_request_restart : restart_soon};
    if (gw_shell_start(&ctx) != ESP_OK) {
        ESP_LOGE(TAG, "console shell unavailable");
    }
}

static void restart_now(void *arg)
{
    (void)arg;
    esp_restart();
}

/* Restart without a Sparkplug session to close (recovery, commissioning): after a short delay
 * so the HTTP response or the shell's answer goes out first. */
static void restart_soon(void)
{
    static esp_timer_handle_t t;
    const esp_timer_create_args_t ta = {.callback = restart_now, .name = "restart"};
    if (t == NULL && esp_timer_create(&ta, &t) != ESP_OK) {
        esp_restart();
    }
    esp_timer_start_once(t, 300 * 1000);
}

static void start_web(gw_web_mode_t mode, const gw_config_t *cfg, const char *source)
{
    if (s_web_started) {
        return;
    }
    s_web_started = true;
    gw_storage_commissioning_password(s_commission_pw);
    gw_secrets_t sec;
    gw_storage_get_secrets(&sec);
    bool no_password = mode == GW_WEB_NORMAL ? cfg->web.password_hash[0] == '\0'
                                              : sec.web_password_hash[0] == '\0';
    memset(&sec, 0, sizeof(sec));
    if (no_password) {
        /* console only (not esp_log): it must not travel to a syslog server */
        printf("web: no web password set; user 'admin', commissioning password '%s'\n",
               s_commission_pw);
    }
    const gw_web_ctx_t ctx = {
        .mode = mode,
        .cfg = cfg,
        .config_source = source,
        .config_error = mode != GW_WEB_NORMAL ? &s_cfg_err : NULL,
        .info = &s_info,
        .commissioning_password = s_commission_pw,
        .restart = mode == GW_WEB_NORMAL ? gw_edge_request_restart : restart_soon,
    };
    if (gw_web_start(&ctx) != ESP_OK) {
        ESP_LOGE(TAG, "web server unavailable");
    }
    if (gw_web_mdns_start(cfg, mode == GW_WEB_NORMAL) != ESP_OK) {   /* NET-6 */
        ESP_LOGE(TAG, "mDNS unavailable");
    }
}

static void net_down_noop(bool up)
{
    gw_status_set(GW_ST_NET_UP, up);
}

static void net_noop(void)
{
}

/* CFG-5 / NET-5: BOOT gestures, in every mode. */
static void on_button(gw_btn_event_t ev)
{
    if (ev == GW_BTN_FACTORY_RESET) {
        gw_storage_factory_reset();
        /* orderly when a session is up (NDEATH): the device leaves the Sparkplug namespace */
        if (s_mode == GW_WEB_NORMAL) {
            gw_edge_request_restart();
        } else {
            restart_soon();
        }
    } else if (ev == GW_BTN_COMMISSIONING) {
        if (!s_ready) {
            ESP_LOGW(TAG, "BOOT gesture ignored: still starting");
            return;
        }
        ESP_LOGW(TAG, "commissioning access point requested with BOOT");
        /* NET-5: the web UI over the access point, also when web.enabled is false */
        start_web(s_mode, &s_cfg, s_source);
        gw_storage_commissioning_password(s_commission_pw);
        printf("ap: password = commissioning password '%s'\n", s_commission_pw);
#if CONFIG_GW_SOFTAP
        gw_ap_start(s_commission_pw, s_mode == GW_WEB_NORMAL ? CONFIG_GW_SOFTAP_TIMEOUT_MIN * 60 : 0);
#else
        gw_ap_start(s_commission_pw, 0);
#endif
    }
}

/* CFG-3 recovery: without a valid configuration nothing that needs one runs (no polling, no
 * MQTT), but the network comes up with the last valid configuration's settings (or DHCP on
 * Ethernet) so the web UI, the REST API and the shell can fix it without reflashing. */
static void run_without_config(gw_web_mode_t mode, const char *source)
{
    s_mode = mode;
    gw_status_set(mode == GW_WEB_COMMISSIONING ? GW_ST_COMMISSIONING : GW_ST_CFG_ERROR, true);
    start_shell(NULL, source);
    gw_storage_recovery_config(&s_cfg);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    const gw_net_callbacks_t net_cb = {
        .ip_changed = net_down_noop, .route_changed = net_noop, .time_synced = net_noop};
    if (gw_net_start(&s_cfg, &net_cb) != ESP_OK) {
        ESP_LOGE(TAG, "recovery network unavailable: only the console shell is left");
    }
    start_web(mode, &s_cfg, source);
    s_source = source;
    s_ready = true;
    gw_ap_start(s_commission_pw, 0);   /* NET-5: no valid configuration */
    for (;;) {
        if (mode == GW_WEB_COMMISSIONING) {
            ESP_LOGW(TAG, "commissioning: no configuration since the factory reset; nothing is "
                          "started (no polling, no MQTT); load one with the web UI on port %u or "
                          "the shell ('config load')",
                     s_cfg.web.port);
        } else {
            ESP_LOGE(TAG, "configuration rejected (%s): %s", source, s_cfg_err.msg);
            ESP_LOGE(TAG, "nothing is started (no polling, no MQTT); fix it with the web UI on "
                          "port %u, the shell ('config load') or by reflashing (see README)",
                     s_cfg.web.port);
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

/* REL-5: logged at boot and published in NBIRTH as "Node Info/Reset Reason". */
static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    case ESP_RST_EFUSE: return "eFuse error";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lock-up";
    default: return "unknown";
    }
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    s_info.firmware = app->version;
    s_info.reset_reason = reset_reason();
    ESP_LOGI(TAG, "%s %s (IDF %s), reset reason: %s", app->project_name, app->version,
             app->idf_ver, s_info.reset_reason);

    gw_port_init();
    gw_tagdb_reset();
    if (gw_status_led_start() != ESP_OK) {
        ESP_LOGE(TAG, "status LED unavailable");
    }
    /* Before the configuration: whatever happens next, no DO stays on from before a reset. */
    if (gw_io_outputs_off() != ESP_OK) {
        ESP_LOGE(TAG, "digital outputs unavailable; DO commands will be refused");
    }
    gw_storage_init();  /* failure is logged; bdSeq then lives in RAM only */

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    snprintf(s_hardware, sizeof(s_hardware), "%s (ESP32-S3 rev %d.%d)", gw_bsp.name,
             chip.revision / 100, chip.revision % 100);
    s_info.hardware = s_hardware;

    if (gw_bootbtn_start(on_button) != ESP_OK) {
        ESP_LOGE(TAG, "BOOT button unavailable: no factory reset");
    }

    const char *source;
    if (gw_storage_commissioning_pending()) {
        snprintf(s_cfg_err.msg, sizeof(s_cfg_err.msg),
                 "sin configuración: puesta en marcha tras un reset de fábrica");
        run_without_config(GW_WEB_COMMISSIONING, "commissioning (factory reset)");
    }
    if (!gw_storage_load_config(&s_cfg, &s_cfg_err, &source)) {
        run_without_config(GW_WEB_RECOVERY, source);
    }
    gw_storage_save_last_good(&s_cfg);
    ESP_LOGI(TAG, "configuration from %s: group '%s', node '%s', %u devices, %u tags", source,
             s_cfg.sparkplug.group_id, s_cfg.sparkplug.edge_node_id, s_cfg.device_count,
             s_cfg.tag_count);
    /* REL-4: from here on the log also goes to the syslog server (buffered until reachable) */
    if (gw_log_start(&s_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "remote syslog unavailable");
    }
    if (s_cfg.sparkplug.primary_host_id[0] != '\0') {
        ESP_LOGW(TAG, "primary_host_id '%s' ignored: SPB-13 is not implemented in this build",
                 s_cfg.sparkplug.primary_host_id);
    }

    ESP_ERROR_CHECK(gw_edge_start(&s_cfg, &s_info));
    if (gw_io_start_inputs(&s_cfg, gw_edge_notify_io) != ESP_OK) {
        ESP_LOGE(TAG, "digital inputs unavailable");
    }

    mb_transport_t tr;
    ESP_ERROR_CHECK(gw_rs485_init(&s_cfg, &tr));
    const gw_bus_callbacks_t bus_cb = {
        .device_updated = on_device_updated,
        .write_done = gw_edge_notify_write_done,
    };
    ESP_ERROR_CHECK(gw_bus_start(&s_cfg, &tr, &bus_cb));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    const gw_net_callbacks_t net_cb = {
        .ip_changed = on_ip_changed,
        .route_changed = gw_edge_notify_route,
        .time_synced = gw_edge_notify_time_synced,
    };
    ESP_ERROR_CHECK(gw_net_start(&s_cfg, &net_cb));
    if (s_cfg.web.enabled) {
        start_web(GW_WEB_NORMAL, &s_cfg, source);
    }
    s_source = source;
    s_ready = true;
    start_shell(&s_cfg, source);
}
