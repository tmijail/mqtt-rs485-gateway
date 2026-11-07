/*
 * RS485 (Modbus RTU master) -> MQTT Sparkplug B 3.0 gateway for the Waveshare
 * ESP32-S3-POE-ETH-8DI-8DO. app_main loads the configuration and wires the
 * components together; all work happens in their tasks:
 *
 *   rs485_bus   (core 1, prio 10)  gw_bus:  sole owner of the RS485 port, polls and writes
 *   sparkplug   (core 0, prio 8)   gw_edge: MQTT session, Sparkplug engine, NCMD/DCMD
 *   esp_timer                      gw_io:   1 kHz DI sampling and debounce
 *   local_io    (prio 9)           gw_io:   sole writer of the DO expander (NCMD, safe state)
 *   status_led  (prio 2)           gw_status
 *   buzzer      (prio 2)           gw_buzzer: alarm patterns and confirmation beeps
 *   console     (prio 2)           gw_shell: diagnostic REPL on the console UART
 *   syslog      (prio 2)           gw_log:   copies the log to a syslog server (if configured)
 *   modbus_tcp  (core 0, prio 3)   gw_mbtcp: Modbus TCP gateway to the RTU slaves (if enabled)
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
#include "gw_buzzer.h"
#include "gw_config.h"
#include "gw_edge.h"
#include "gw_fw.h"
#include "gw_io.h"
#include "gw_localio.h"
#include "gw_log.h"
#include "gw_mbtcp.h"
#include "gw_net.h"
#include "gw_port.h"
#include "gw_rs485.h"
#include "gw_shell.h"
#include "gw_status.h"
#include "gw_storage.h"
#include "gw_tagdb.h"
#include "gw_web.h"

static const char *TAG = "main";

#define NVS_DO_HOLD "do_hold"   /* IO-3: outputs the last valid configuration holds */
#define NVS_ALARM_CFG "alarm_cfg"   /* IO-6: the last valid configuration wants the
                                     * "config_error" alarm (a rejected one cannot say) */

/* Immutable after boot; shared read-only by every task. About 32 KB: allocated at boot in
 * PSRAM when the board has it (internal RAM otherwise, e.g. QEMU), see big_alloc(). */
static gw_config_t *s_cfg;
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

/* TIME-1/TIME-2: every SNTP synchronisation also sets the RTC. */
static void on_time_synced(void)
{
    gw_edge_notify_time_synced();
    gw_io_rtc_store();
}

static void on_device_updated(uint16_t dev)
{
    gw_edge_notify_device(dev);
}

static void restart_soon(void);

static bool s_shell_ok;

/* Issue #72: the shell starts first thing, so whatever is typed during the boot gets an
 * answer; commands that need a subsystem not running yet say so (gw_shell_mark_up). */
static void start_shell_early(void)
{
    const gw_shell_ctx_t ctx = {.starting = true, .config_source = "", .config_error = "",
                                .info = &s_info, .restart = restart_soon};
    s_shell_ok = gw_shell_start(&ctx) == ESP_OK;
    if (!s_shell_ok) {
        ESP_LOGE(TAG, "console shell unavailable");
    }
}

/* The shell's view of the gateway: the configuration (NULL if rejected), and whether the
 * Sparkplug task runs (until then a restart needs no NDEATH). */
static void start_shell(const gw_config_t *cfg, const char *source, bool running)
{
    const gw_shell_ctx_t ctx = {.cfg = cfg,
                                .starting = cfg != NULL && !running,
                                .commissioning = s_mode == GW_WEB_COMMISSIONING,
                                .config_source = source,
                                .config_error = s_cfg_err.msg,
                                .info = &s_info,
                                .restart = cfg != NULL && running ? gw_edge_request_restart
                                                                  : restart_soon};
    if (s_shell_ok) {
        gw_shell_set_ctx(&ctx);
    }
}

static void restart_now(void *arg)
{
    (void)arg;
    gw_fw_restart_requested();   /* issue #108 B5 */
    gw_bus_quiesce(1000);   /* no-op unless the bus runs */
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

/* Not even the configuration structure fits (no PSRAM, internal heap exhausted): nothing can
 * run, not even the recovery network; the shell reports why. */
static void halt_no_memory(void)
{
    /* issue #99: a new image on trial goes back to the previous one at once (no trial timer
     * runs yet, nothing else would ever judge it) */
    gw_fw_cannot_start("no memory for the configuration");
    snprintf(s_cfg_err.msg, sizeof(s_cfg_err.msg), "sin memoria para la configuración (%u B)",
             (unsigned)sizeof(gw_config_t));
    gw_status_set(GW_ST_CFG_ERROR, true);
    start_shell(NULL, "RAM", false);
    for (;;) {
        ESP_LOGE(TAG, "%s: nothing is started", s_cfg_err.msg);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

static void net_down_noop(bool up)
{
    gw_status_set(GW_ST_NET_UP, up);
}

static void net_noop(void)
{
}

/* NET-5: the access point, with the web UI's address as the web server really serves it
 * (HTTPS, or HTTP when web.https is false or the device certificate is missing). */
static void start_ap(uint32_t timeout_s)
{
    gw_web_stats_t ws;
    gw_web_get_stats(&ws);
    gw_ap_start(s_commission_pw, timeout_s, ws.https,
                ws.https ? s_cfg->web.https_port : s_cfg->web.port);
}

/* CFG-5 / NET-5: BOOT gestures, in every mode. */
static void on_button(gw_btn_event_t ev)
{
    if (ev == GW_BTN_FACTORY_RESET) {
        gw_storage_factory_reset();
        /* orderly when a session is up (NDEATH): the device leaves the Sparkplug namespace */
        if (s_mode == GW_WEB_NORMAL) {
            gw_edge_request_restart_discard();   /* #136: the RAM ring is not kept */
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
        start_web(s_mode, s_cfg, s_source);
        gw_storage_commissioning_password(s_commission_pw);
        printf("ap: password = commissioning password '%s'\n", s_commission_pw);
#if CONFIG_GW_SOFTAP
        start_ap(s_mode == GW_WEB_NORMAL ? CONFIG_GW_SOFTAP_TIMEOUT_MIN * 60 : 0);
#else
        start_ap(0);
#endif
    }
}

/* CFG-3 recovery: without a valid configuration nothing that needs one runs (no polling, no
 * MQTT), but the network comes up with the last valid configuration's settings (or DHCP on
 * Ethernet) so the web UI, the REST API and the shell can fix it without reflashing. */
static void run_without_config(gw_web_mode_t mode, const char *source)
{
    s_mode = mode;
    gw_io_outputs_all_off();   /* IO-3: "hold" belonged to a configuration that is not valid */
    /* issue #74: an expander that did not answer at boot is retried here too, to switch off
     * whatever its latch kept from before a warm reset */
    if (gw_io_start_recovery() != ESP_OK) {
        ESP_LOGE(TAG, "local I/O task unavailable: the output expander is not retried");
    }
    uint8_t alarm = 0;
    if (mode == GW_WEB_RECOVERY && gw_storage_get_blob(NVS_ALARM_CFG, &alarm, 1) && alarm) {
        gw_buzzer_configure(GW_ALARM_BIT(GW_ALARM_CONFIG_ERROR), 0);   /* IO-6 */
    }
    gw_status_set(mode == GW_WEB_COMMISSIONING ? GW_ST_COMMISSIONING : GW_ST_CFG_ERROR, true);
    start_shell(NULL, source, false);
    gw_storage_recovery_config(s_cfg);
    /* FW-1: web uploads stay possible (the way out of a bad configuration); a new image on
     * trial that rejects the configuration goes back to the previous one at once */
    const gw_fw_hooks_t fw_hooks = {.changed = NULL, .restart = restart_soon};
    gw_fw_init(s_cfg, true, &fw_hooks);
    if (mode == GW_WEB_COMMISSIONING) {
        gw_fw_commissioning();
    } else {
        gw_fw_config_rejected();
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    const gw_net_callbacks_t net_cb = {
        .ip_changed = net_down_noop, .route_changed = net_noop,
        .time_synced = gw_io_rtc_store};   /* TIME-2: the RTC follows SNTP here too */
    if (gw_net_start(s_cfg, &net_cb) != ESP_OK) {
        ESP_LOGE(TAG, "recovery network unavailable: only the console shell is left");
    } else {
        gw_shell_mark_up(GW_SHELL_UP_NET);
    }
    start_web(mode, s_cfg, source);
    s_source = source;
    s_ready = true;
    start_ap(0);   /* NET-5: no valid configuration */
    for (;;) {
        if (mode == GW_WEB_COMMISSIONING) {
            ESP_LOGW(TAG, "commissioning: no configuration since the factory reset; nothing is "
                          "started (no polling, no MQTT); load one with the web UI on port %u or "
                          "the shell ('config load')",
                     s_cfg->web.https ? s_cfg->web.https_port : s_cfg->web.port);
        } else {
            ESP_LOGE(TAG, "configuration rejected (%s): %s", source, s_cfg_err.msg);
            ESP_LOGE(TAG, "nothing is started (no polling, no MQTT); fix it with the web UI on "
                          "port %u, the shell ('config load') or by reflashing (see README)",
                     s_cfg->web.https ? s_cfg->web.https_port : s_cfg->web.port);
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
    gw_storage_init();  /* failure is logged; bdSeq then lives in RAM only */
    /* Before the configuration: whatever happens next, no DO stays on from before a reset,
     * except those the last valid configuration holds across a restart (IO-3, kept in NVS). */
    uint8_t hold = 0;
    gw_storage_get_blob(NVS_DO_HOLD, &hold, sizeof(hold));
    if (gw_io_outputs_boot(hold) != ESP_OK) {
        ESP_LOGE(TAG, "digital outputs unavailable: published as null and DO commands refused "
                      "until the expander answers");
    }
    gw_io_rtc_restore();   /* TIME-2: a valid time before the network, if the RTC kept it */

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    snprintf(s_hardware, sizeof(s_hardware), "%s (ESP32-S3 rev %d.%d)", gw_bsp.name,
             chip.revision / 100, chip.revision % 100);
    s_info.hardware = s_hardware;

    /* IO-6: GPIO46 straps the boot: configured only now, after the boot */
    if (gw_buzzer_start() != ESP_OK) {
        ESP_LOGE(TAG, "buzzer unavailable");
    }
    if (gw_bootbtn_start(on_button) != ESP_OK) {
        ESP_LOGE(TAG, "BOOT button unavailable: no factory reset");
    }
    gw_fw_early_init();   /* FW-1: before the shell can ask for the update status (#101) */
    start_shell_early();

    const char *source;
    s_cfg = gw_storage_big_alloc(sizeof(*s_cfg), "configuration");
    if (s_cfg == NULL) {
        halt_no_memory();
    }
    if (gw_storage_commissioning_pending()) {
        snprintf(s_cfg_err.msg, sizeof(s_cfg_err.msg),
                 "sin configuración: puesta en marcha tras un reset de fábrica");
        run_without_config(GW_WEB_COMMISSIONING, "commissioning (factory reset)");
    }
    if (!gw_storage_load_config(s_cfg, &s_cfg_err, &source)) {
        run_without_config(GW_WEB_RECOVERY, source);
    }
    gw_storage_save_last_good(s_cfg);
    start_shell(s_cfg, source, false);
    hold = gw_lio_hold_mask(s_cfg);   /* for the next restart's first step */
    if (gw_storage_set_blob(NVS_DO_HOLD, &hold, sizeof(hold)) != ESP_OK) {
        ESP_LOGW(TAG, "DO hold mask not stored: the next restart switches every DO off first");
    }
    uint8_t alarm = (s_cfg->local_io.buzzer.alarms & GW_ALARM_BIT(GW_ALARM_CONFIG_ERROR)) != 0;
    gw_storage_set_blob(NVS_ALARM_CFG, &alarm, sizeof(alarm));
    gw_buzzer_configure(s_cfg->local_io.buzzer.alarms, s_cfg->local_io.buzzer.alarm_after_s);
    ESP_LOGI(TAG, "configuration from %s: group '%s', node '%s', %u devices, %u tags", source,
             s_cfg->sparkplug.group_id, s_cfg->sparkplug.edge_node_id, s_cfg->device_count,
             s_cfg->tag_count);
    /* REL-4: from here on the log also goes to the syslog server (buffered until reachable) */
    if (gw_log_start(s_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "remote syslog unavailable");
    }
    /* FW-1: before anything can judge a new image (its trial starts now) */
    const gw_fw_hooks_t fw_hooks = {.changed = gw_edge_notify_firmware,
                                    .restart = gw_edge_request_restart};
    gw_fw_init(s_cfg, s_cfg->ota.enabled, &fw_hooks);
    ESP_ERROR_CHECK(gw_edge_start(s_cfg, &s_info));
    gw_shell_mark_up(GW_SHELL_UP_EDGE);
    if (gw_io_start(s_cfg, gw_edge_notify_io) != ESP_OK) {
        ESP_LOGE(TAG, "digital inputs unavailable");
    } else {
        gw_shell_mark_up(GW_SHELL_UP_IO);
    }

    mb_transport_t tr;
    ESP_ERROR_CHECK(gw_rs485_init(s_cfg, &tr));
    const gw_bus_callbacks_t bus_cb = {
        .device_updated = on_device_updated,
        .write_done = gw_edge_notify_write_done,
    };
    ESP_ERROR_CHECK(gw_bus_start(s_cfg, &tr, &bus_cb));
    gw_shell_mark_up(GW_SHELL_UP_BUS);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    const gw_net_callbacks_t net_cb = {
        .ip_changed = on_ip_changed,
        .route_changed = gw_edge_notify_route,
        .time_synced = on_time_synced,
    };
    ESP_ERROR_CHECK(gw_net_start(s_cfg, &net_cb));
    gw_shell_mark_up(GW_SHELL_UP_NET);
    if (gw_mbtcp_start(s_cfg) != ESP_OK) {   /* MB-15, only with modbus_tcp.enabled */
        ESP_LOGE(TAG, "Modbus TCP gateway unavailable");
    }
    if (s_cfg->web.enabled) {
        start_web(GW_WEB_NORMAL, s_cfg, source);
    }
    s_source = source;
    s_ready = true;
    start_shell(s_cfg, source, true);
}
