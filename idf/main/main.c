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
 *   + IDF tasks: esp-mqtt, lwIP tcpip, esp_eth, event loop
 */
#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gw_bsp.h"
#include "gw_bus.h"
#include "gw_config.h"
#include "gw_edge.h"
#include "gw_io.h"
#include "gw_net.h"
#include "gw_port.h"
#include "gw_rs485.h"
#include "gw_shell.h"
#include "gw_status.h"
#include "gw_storage.h"
#include "gw_tagdb.h"

static const char *TAG = "main";

static gw_config_t s_cfg;   /* immutable after boot; shared read-only by every task */
static char s_hardware[64];
static gw_node_info_t s_info;  /* NBIRTH "Node Info/..." */
static gw_cfg_err_t s_cfg_err;

static void on_device_updated(uint16_t dev)
{
    gw_edge_notify_device(dev);
}

static void start_shell(const gw_config_t *cfg, const char *source)
{
    const gw_shell_ctx_t ctx = {
        .cfg = cfg, .config_source = source, .config_error = s_cfg_err.msg, .info = &s_info};
    if (gw_shell_start(&ctx) != ESP_OK) {
        ESP_LOGE(TAG, "console shell unavailable");
    }
}

static void halt_with_config_error(const gw_cfg_err_t *err, const char *source)
{
    gw_status_set(GW_ST_CFG_ERROR, true);
    start_shell(NULL, source);  /* diagnostics stay available */
    for (;;) {
        ESP_LOGE(TAG, "configuration rejected (%s): %s", source, err->msg);
        ESP_LOGE(TAG, "nothing is started (no polling, no MQTT); fix it and reflash (see README)");
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

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    snprintf(s_hardware, sizeof(s_hardware), "%s (ESP32-S3 rev %d.%d)", gw_bsp.name,
             chip.revision / 100, chip.revision % 100);
    s_info.hardware = s_hardware;

    const char *source;
    if (!gw_storage_load_config(&s_cfg, &s_cfg_err, &source)) {
        halt_with_config_error(&s_cfg_err, source);
    }
    ESP_LOGI(TAG, "configuration from %s: group '%s', node '%s', %u devices, %u tags", source,
             s_cfg.sparkplug.group_id, s_cfg.sparkplug.edge_node_id, s_cfg.device_count,
             s_cfg.tag_count);
    if (s_cfg.sparkplug.primary_host_id[0] != '\0') {
        ESP_LOGW(TAG, "primary_host_id '%s' ignored: SPB-13 is not implemented in this build",
                 s_cfg.sparkplug.primary_host_id);
    }

    ESP_ERROR_CHECK(gw_edge_start(&s_cfg, &s_info));
    if (gw_io_init(&s_cfg, gw_edge_notify_io) != ESP_OK) {
        /* Keep the gateway running: DIs are still published, DO commands are refused. */
        ESP_LOGE(TAG, "digital outputs unavailable");
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
        .ip_changed = gw_edge_notify_ip,
        .time_synced = gw_edge_notify_time_synced,
    };
    ESP_ERROR_CHECK(gw_net_start(&s_cfg, &net_cb));
    start_shell(&s_cfg, source);
}
