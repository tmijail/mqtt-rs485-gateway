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
#include "gw_coredump.h"
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
#include "gw_temp.h"
#include "gw_web.h"
#include "gw_wpa.h"

static const char *TAG = "main";

#define NVS_DO_HOLD "do_hold"   /* IO-3: outputs the last valid configuration holds */
#define NVS_ALARM_CFG "alarm_cfg"   /* IO-6: the last valid configuration wants the
                                     * "config_error" alarm (a rejected one cannot say) */

/* Immutable after boot; shared read-only by every task. About 32 KB: allocated at boot in
 * PSRAM (internal RAM in the fault mode and in the QEMU build without PSRAM), see big_alloc(). */
static gw_config_t *s_cfg;
static char s_hardware[64];
static gw_node_info_t s_info;  /* NBIRTH "Node Info/..." */
static gw_cfg_err_t s_cfg_err;
static char s_commission_pw[GW_COMMISSION_PW_LEN + 1];
static gw_web_mode_t s_mode = GW_WEB_NORMAL;
static bool s_web_started;
static const char *s_source = "";
static volatile bool s_ready;   /* the mode is settled: BOOT gestures may act on it */
static bool s_safe;             /* issue #144 item 4: safe mode (repeated crashes) */
static bool s_net_started;      /* the network driver started (the self-check of FW-1) */

#define SAFE_MODE_MSG "safe mode: %lu crash resets in a row; the configuration is not loaded. " \
                      "Normal mode again in %d min, at a restart by the operator or a power cycle"
/* issue #209 */
#define SAFE_MODE_REQ_MSG "safe mode on request: the configuration is not loaded. Normal mode " \
                          "again in %d min, at a restart or a power cycle"

/* Issue #135: the firmware assumes the board's PSRAM. Without it the gateway does not start
 * (GW_WEB_FAULT): the log, the shell and the web UI give this text. */
static const char PSRAM_FAULT[] = "PSRAM not detected: the board has 8 MB of PSRAM and the boot "
                                  "did not find it (a defective or missing chip); the gateway "
                                  "does not start";

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
static esp_err_t factory_reset_remote(void);

static bool s_shell_ok;

/* Issue #72: the shell starts first thing, so whatever is typed during the boot gets an
 * answer; commands that need a subsystem not running yet say so (gw_shell_mark_up). */
static void start_shell_early(void)
{
    const gw_shell_ctx_t ctx = {.starting = true, .config_source = "", .config_error = "",
                                .info = &s_info, .restart = restart_soon,
                                .factory_reset = factory_reset_remote};
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
                                .fault = s_mode == GW_WEB_FAULT ? PSRAM_FAULT : NULL,
                                .config_source = source,
                                .config_error = s_cfg_err.msg,
                                .info = &s_info,
                                .restart = cfg != NULL && running ? gw_edge_request_restart
                                                                  : restart_soon,
                                .factory_reset = factory_reset_remote};
    if (s_shell_ok) {
        gw_shell_set_ctx(&ctx);
    }
}

static void restart_now(void *arg)
{
    (void)arg;
    gw_fw_restart_requested();   /* issue #99; #144: an operator restart clears the counter */
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

/* #203: the restart after a factory reset from the web UI, the REST API or the shell: the same
 * as after BOOT held 10 s (orderly NDEATH when a session is up; #136: the RAM ring is not kept) */
static void factory_restart(void *arg)
{
    (void)arg;
    if (s_mode == GW_WEB_NORMAL && s_ready) {
        gw_edge_request_restart_discard();
    } else {
        restart_soon();
    }
}

/* #203: the factory reset of the web UI, the REST API and the shell: the LED and the two beeps of
 * the BOOT gesture, the erase (gw_storage_factory_reset), then the restart into commissioning
 * 0.5 s later, so that the answer goes out first. ESP_ERR_INVALID_STATE: refused, nothing erased
 * and no restart. Another error: as BOOT, the gateway restarts (the erase went as far as it could). */
static esp_err_t factory_reset_remote(void)
{
    static esp_timer_handle_t t;
    gw_status_set(GW_ST_RESETTING, true);
    gw_buzzer_beep_wait(2);   /* IO-6: the confirmation of a factory reset */
    esp_err_t e = gw_storage_factory_reset();
    if (e == ESP_ERR_INVALID_STATE) {
        gw_status_set(GW_ST_RESETTING, false);
        return e;
    }
    const esp_timer_create_args_t ta = {.callback = factory_restart, .name = "factory_restart"};
    if (t == NULL && esp_timer_create(&ta, &t) != ESP_OK) {
        factory_restart(NULL);
        return e;
    }
    esp_timer_start_once(t, 500 * 1000);
    return e;
}

/* Issue #144 item 1: the self-check items of a new image on trial (gw_fw asks about once a
 * second). Not the link, not the broker: only what this firmware itself starts. */
static void self_check(gw_ota_selfcheck_t *out)
{
    out->net_started = s_net_started;
    out->web_needed = s_web_started;   /* a web server was started (or failed to start) */
    out->web_alive = gw_web_loop_alive(GW_OTA_WEB_PING_MS);
}

/* NET-5, issue #187: the WPA2 key of the access point: the PMK of the web password (64
 * hexadecimal characters) when one is stored, else the commissioning password.
 * Issue #248: a stored web password with no valid PMK is a fault of the stored secrets, not a
 * format: gw_storage writes the PMK with each web password. The PMK is absent only when its
 * calculation failed (the MAC address was not readable) or when its NVS entry was not written
 * after the entry of the hash (a write error, a power cut between the two). The key is then the
 * commissioning password too, with a warning: the owner can still use the access point. Only a
 * new web password gives a PMK again, because the gateway does not keep the password.
 * Issue #269: a PMK is the key only with the hash of a web password. gw_storage erases the PMK
 * before it changes or removes the hash, so a stored PMK is the PMK of the stored password; a
 * PMK with no web password is not used. */
static bool ap_key(char out[GW_AP_KEY_SIZE])
{
    gw_storage_commissioning_password(s_commission_pw);
    gw_secrets_t sec;
    gw_storage_get_secrets(&sec);
    bool pmk = gw_wpa_key_is_pmk(sec.web_password_hash, sec.ap_pmk);
    if (!pmk && sec.web_password_hash[0] != '\0') {
        ESP_LOGW(TAG, "access point: a web password is set, but its WPA2 key (PMK) is not in the "
                      "stored secrets: the key is the commissioning password. Set the web "
                      "password again to correct this");
    }
    snprintf(out, GW_AP_KEY_SIZE, "%s", pmk ? sec.ap_pmk : s_commission_pw);
    memset(&sec, 0, sizeof(sec));
    return pmk;
}

/* CFG-3 / NET-5, issue #120: the claim window closed (time over, a password was set, or the
 * access point stopped): the LED goes back; a set password gets one beep at the panel. Issue
 * #187: after a claim, the access point changes to WPA2 with the new web password a few seconds
 * after the answer (gw_ap_claimed); at the end of the window time its open period ends too. */
static void claim_closed(bool claimed)
{
    gw_status_set(GW_ST_CLAIM, false);
    if (claimed) {
        gw_buzzer_beep(1);
        char key[GW_AP_KEY_SIZE];
        bool pmk = ap_key(key);
        gw_ap_claimed(key, pmk);
        memset(key, 0, sizeof(key));
    }
}

/* NET-5, issue #187: the access point started, stopped or changed its key: the LED patterns. A
 * stop also closes the claim window: it accepts only clients of the access point. */
static void ap_changed(bool running, bool open, gw_ap_stop_t why)
{
    gw_status_set(GW_ST_AP, running);
    gw_status_set(GW_ST_AP_OPEN, running && open);
    if (!running) {
        gw_web_claim_close(why == GW_AP_STOP_IDLE ? "the access point stopped (no client)"
                                                  : "the access point stopped");
    }
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
        .fault = mode == GW_WEB_FAULT ? PSRAM_FAULT : NULL,
        .info = &s_info,
        .commissioning_password = s_commission_pw,
        .restart = mode == GW_WEB_NORMAL ? gw_edge_request_restart : restart_soon,
        .factory_reset = factory_reset_remote,
        .ap_ip4 = gw_ap_ip4,
        .claim_closed = claim_closed,
    };
    if (gw_web_start(&ctx) != ESP_OK) {
        ESP_LOGE(TAG, "web server unavailable");
    }
    if (gw_web_mdns_start(cfg, mode == GW_WEB_NORMAL) != ESP_OK) {   /* NET-6 */
        ESP_LOGE(TAG, "mDNS unavailable");
    }
}

/* Not even the configuration structure fits (the heap is exhausted): nothing can run, not even
 * the recovery network; the shell reports why. */
static void halt_no_memory(void)
{
    /* issue #144: a new image on trial goes back to the earlier one at once */
    gw_fw_cannot_start("no memory to start");
    snprintf(s_cfg_err.msg, sizeof(s_cfg_err.msg), "no memory for the configuration (%u B)",
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
 * (HTTPS, or HTTP when web.https is false or the device certificate is missing). @p max_s: its
 * longest time (0 = none). @p open_s: the open period of a claim window (issue #120), 0 = WPA2.
 * Issue #187: the WPA2 key is the web password (its PMK), else the commissioning password. */
static esp_err_t start_ap(uint32_t max_s, uint32_t open_s)
{
    gw_web_stats_t ws;
    gw_web_get_stats(&ws);
    char key[GW_AP_KEY_SIZE];
    bool pmk = ap_key(key);
    esp_err_t e = gw_ap_start(key, pmk, max_s, ws.https,
                              ws.https ? s_cfg->web.https_port : s_cfg->web.port, open_s);
    memset(key, 0, sizeof(key));
    if (e == ESP_OK && !pmk) {
        /* console only (not esp_log): it must not travel to a syslog server */
        printf("ap: WPA2 key = commissioning password '%s'\n", s_commission_pw);
    }
    return e;
}

#if CONFIG_GW_SOFTAP || CONFIG_GW_IO_SIMULATED
#define AP_MAX_NORMAL_S (CONFIG_GW_SOFTAP_TIMEOUT_MIN * 60u)
#else
#define AP_MAX_NORMAL_S 0u
#endif

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
            /* issue #187: no beep, so the operator knows that the gesture did nothing */
            ESP_LOGW(TAG, "BOOT gesture ignored: still starting (no beep); push BOOT again "
                          "when the boot is complete");
            return;
        }
        gw_buzzer_beep(1);   /* IO-6: the gesture is accepted */
        ESP_LOGW(TAG, "commissioning access point requested with BOOT");
        /* NET-5: the web UI over the access point, also when web.enabled is false */
        start_web(s_mode, s_cfg, s_source);
        /* CFG-3 / NET-5, issue #120: while no web password is set, the same gesture opens the
         * claim window, and the access point is open (no key) for the time of the window */
        uint32_t claim_s = gw_web_claim_window_s();
        esp_err_t ap = start_ap(s_mode == GW_WEB_NORMAL ? AP_MAX_NORMAL_S : 0, claim_s);
#if CONFIG_GW_IO_SIMULATED
        /* QEMU has no radio: the window opens without the access point, so the E2E can examine
         * it (a claim over Ethernet is refused unless the shell's 'claimsim ... eth' says so) */
        const bool need_ap = false;
#else
        const bool need_ap = true;
#endif
        if (claim_s == 0) {
            ESP_LOGI(TAG, "claim window not opened: a web password is set (or no web server)");
        } else if (ap != ESP_OK && need_ap) {
            /* the window accepts only clients of the access point: without it, nobody */
            ESP_LOGW(TAG, "claim window not opened: no access point (%s); set the web password "
                          "with the commissioning password of the serial console",
                     esp_err_to_name(ap));
        } else if (gw_web_claim_open(claim_s)) {
            gw_status_set(GW_ST_CLAIM, true);
        }
    }
}

/* CFG-3 recovery: without a valid configuration nothing that needs one runs (no polling, no
 * MQTT), but the network comes up with the last valid configuration's settings (or DHCP on
 * Ethernet) so the web UI, the REST API and the shell can fix it without reflashing.
 * #135: the fault mode (no PSRAM) is the same, with the configuration not loaded at all: only
 * what reports the fault runs, from internal RAM. */
static void run_without_config(gw_web_mode_t mode, const char *source)
{
    s_mode = mode;
    gw_io_outputs_all_off();   /* IO-3: "hold" belonged to a configuration that is not valid */
    /* issue #74: an expander that did not answer at boot is retried here too, to switch off
     * whatever its latch kept from before a warm reset */
    if (gw_io_start_recovery() != ESP_OK) {
        ESP_LOGE(TAG, "local I/O task unavailable: the output expander is not retried");
    }
    /* #209: safe mode (on request or after crashes) did not reject the configuration: no
     * "config_error" alarm and no configuration error state on the LED, but a pattern of its own */
    const bool safe = mode == GW_WEB_RECOVERY && s_safe;
    uint8_t alarm = 0;
    if (mode == GW_WEB_RECOVERY && !safe && gw_storage_get_blob(NVS_ALARM_CFG, &alarm, 1) &&
        alarm) {
        gw_buzzer_configure(GW_ALARM_BIT(GW_ALARM_CONFIG_ERROR), 0);   /* IO-6 */
    }
    /* #135: the fault mode shows the configuration error state on the LED too */
    gw_status_set(mode == GW_WEB_COMMISSIONING ? GW_ST_COMMISSIONING
                  : safe                       ? GW_ST_SAFE_MODE
                                               : GW_ST_CFG_ERROR,
                  true);
    start_shell(NULL, source, false);
    gw_storage_recovery_config(s_cfg);
    /* FW-1: web uploads stay possible (the way out of a bad configuration). Issue #144 item 2:
     * a new image installed from normal mode that rejects the configuration goes back to the
     * earlier one at once; one installed from recovery, safe mode or commissioning (or forced)
     * stays, and the self-check confirms it */
    const gw_fw_hooks_t fw_hooks = {.changed = NULL, .restart = restart_soon,
                                    .self_check = self_check};
    gw_fw_init(s_cfg, false, &fw_hooks);
    if (mode == GW_WEB_COMMISSIONING) {
        gw_fw_mode(GW_OTA_MODE_COMMISSIONING, NULL);
    } else if (mode == GW_WEB_FAULT) {
        gw_fw_cannot_start("no memory to start (no PSRAM)");   /* the image cannot run */
    } else {
        char where[64];   /* the cause in fw_last is short (GW_OTA_CAUSE_MAX) */
        if (s_cfg_err.path[0] != '\0') {
            snprintf(where, sizeof(where), "%.63s", s_cfg_err.path);
        } else if (s_cfg_err.line != 0) {
            snprintf(where, sizeof(where), "line %u, column %u", s_cfg_err.line,
                     s_cfg_err.column);
        } else {
            snprintf(where, sizeof(where), "%.63s", s_safe ? "safe mode" : s_cfg_err.msg);
        }
        gw_fw_mode(GW_OTA_MODE_RECOVERY, where);
        if (s_safe) {
            gw_fw_safe_mode();   /* normal mode again after CONFIG_GW_FW_SAFE_MODE_MIN */
        }
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    const gw_net_callbacks_t net_cb = {
        .ip_changed = net_down_noop, .route_changed = net_noop,
        .time_synced = on_time_synced};   /* TIME-2: the RTC follows SNTP here too; #160: the
                                           * clock source in /api/status (no browser time over it) */
    if (gw_net_start(s_cfg, &net_cb) != ESP_OK) {
        ESP_LOGE(TAG, "recovery network unavailable: only the console shell is left");
    } else {
        s_net_started = true;
        gw_shell_mark_up(GW_SHELL_UP_NET);
    }
    start_web(mode, s_cfg, source);
    s_source = source;
    s_ready = true;
    start_ap(0, 0);   /* NET-5: no valid configuration */
    for (;;) {
        if (mode == GW_WEB_FAULT) {
            ESP_LOGE(TAG, "%s (issue #135)", PSRAM_FAULT);
            ESP_LOGE(TAG, "nothing is started (no polling, no MQTT): only the network, the web UI "
                          "on port %u and the shell, to report the fault",
                     s_cfg->web.https ? s_cfg->web.https_port : s_cfg->web.port);
        } else if (mode == GW_WEB_COMMISSIONING) {
            ESP_LOGW(TAG, "commissioning: no configuration since the factory reset; nothing is "
                          "started (no polling, no MQTT); load one with the web UI on port %u or "
                          "the shell ('config load')",
                     s_cfg->web.https ? s_cfg->web.https_port : s_cfg->web.port);
        } else if (s_safe) {
            ESP_LOGE(TAG, "%s", s_cfg_err.msg);
            ESP_LOGE(TAG, "safe mode: nothing is started (no polling, no MQTT); the web UI on "
                          "port %u and the shell accept a firmware upload",
                     s_cfg->web.https ? s_cfg->web.https_port : s_cfg->web.port);
        } else {
            ESP_LOGE(TAG, "configuration rejected (%s): %s", source, s_cfg_err.msg);
            ESP_LOGE(TAG, "nothing is started (no polling, no MQTT); fix it with the web UI on "
                          "port %u, the shell ('config load') or by reflashing (docs/how-to/recover-the-gateway.md)",
                     s_cfg->web.https ? s_cfg->web.https_port : s_cfg->web.port);
        }
        for (int i = 0; i < 10; i++) {   /* issue #144: the self-check of a new image on trial */
            gw_fw_trial_poll();
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
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
    /* issue #144 item 4: the boot-loop counter, before anything that can crash in a loop (the
     * storage, the configuration) */
    s_safe = gw_fw_bootloop_start();
    if (s_safe) {
        gw_storage_safe_mode();   /* no NVS migration */
    }

    gw_port_init();
    gw_tagdb_reset();
    if (gw_ap_init(ap_changed) != ESP_OK) {   /* NET-5, issue #187: before any BOOT gesture */
        ESP_LOGE(TAG, "commissioning access point unavailable");
    }
    if (gw_status_led_start() != ESP_OK) {
        ESP_LOGE(TAG, "status LED unavailable");
    }
    gw_storage_init();  /* failure is logged; no stored secrets then */
    /* Issue #266: a factory reset that a power cut stopped is completed here, before anything
     * reads the NVS or the configuration; then a restart, so that the boot into commissioning
     * mode starts from the erased storage. When a step fails again, the reset stays pending and
     * gw_storage_load_config() rejects: recovery mode. Safe mode (repeated crashes) does not do
     * the steps, which can be what crashes; it loads no configuration in any case. */
    if (!s_safe && gw_storage_factory_reset_pending()) {
        gw_status_set(GW_ST_RESETTING, true);
        if (gw_storage_factory_reset_resume() == ESP_OK) {
            ESP_LOGW(TAG, "factory reset completed: restarting into commissioning mode");
            esp_restart();
        }
        gw_status_set(GW_ST_RESETTING, false);
    }
    /* #200: the live log of the web UI keeps the log lines from here on (PSRAM) */
    if (gw_log_ring_start() != ESP_OK) {
        ESP_LOGE(TAG, "live log of the web UI unavailable");
    }
    gw_temp_start();   /* #212: the chip temperature (an error is logged) */
    gw_coredump_init();   /* #132: before the shell, the web server and the Sparkplug task */
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
#if !CONFIG_GW_EMU_NO_PSRAM
    if (!gw_storage_psram()) {
        /* #135: CONFIG_SPIRAM_IGNORE_NOTFOUND lets the boot go on without the PSRAM only so
         * that the fault can be reported; the gateway does not start */
        ESP_LOGE(TAG, "%s (issue #135)", PSRAM_FAULT);
        run_without_config(GW_WEB_FAULT, "not loaded (hardware fault)");
    }
#endif
    if (s_safe) {
        bool requested;
        uint32_t left_s;
        gw_fw_safe_mode_info(&requested, &left_s);
        if (requested) {   /* issue #209 */
            snprintf(s_cfg_err.msg, sizeof(s_cfg_err.msg), SAFE_MODE_REQ_MSG,
                     CONFIG_GW_FW_SAFE_MODE_MIN);
        } else {
            snprintf(s_cfg_err.msg, sizeof(s_cfg_err.msg), SAFE_MODE_MSG,
                     (unsigned long)gw_fw_boot_count(), CONFIG_GW_FW_SAFE_MODE_MIN);
        }
        run_without_config(GW_WEB_RECOVERY, requested ? "not loaded (safe mode: on request)"
                                                      : "not loaded (safe mode: repeated crashes)");
    }
    if (gw_storage_commissioning_pending()) {
        snprintf(s_cfg_err.msg, sizeof(s_cfg_err.msg),
                 "no configuration: commissioning after a factory reset");
        run_without_config(GW_WEB_COMMISSIONING, "commissioning (factory reset)");
    }
    if (!gw_storage_load_config(s_cfg, &s_cfg_err, &source)) {
        run_without_config(GW_WEB_RECOVERY, source);
    }
    if (!gw_fw_on_trial()) {
        /* issue #144 item 8: a new image on trial writes it after its confirmation */
        gw_storage_save_last_good(s_cfg);
    }
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
    /* FW-1: before the mode is reported (the trial of a new image counts from the boot) */
    const gw_fw_hooks_t fw_hooks = {.changed = gw_edge_notify_firmware,
                                    .restart = gw_edge_request_restart,
                                    .self_check = self_check};
    gw_fw_init(s_cfg, s_cfg->ota.mqtt, &fw_hooks);
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
    s_net_started = true;
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
    /* issue #144: normal mode reached; a new image on trial waits here for its self-check (this
     * task is not watched by the task watchdog: gw_fw's backstop timer covers a block) */
    gw_fw_mode(GW_OTA_MODE_NORMAL, NULL);
    gw_fw_trial_wait();
}
