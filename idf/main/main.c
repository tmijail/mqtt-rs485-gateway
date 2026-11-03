/*
 * RS485 (Modbus RTU master) -> MQTT Sparkplug B 3.0 gateway for the Waveshare
 * ESP32-S3-POE-ETH-8DI-8DO. app_main loads the configuration and wires the
 * components together; all work happens in their tasks:
 *
 *   rs485_bus   (core 1, prio 10)  gw_bus:  sole owner of the RS485 port, polls and writes
 *   sparkplug   (core 0, prio 8)   gw_edge: MQTT session, Sparkplug engine, NCMD/DCMD
 *   esp_timer                      gw_io:   1 kHz DI sampling and debounce
 *   status_led  (prio 2)           gw_status
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
#include "gw_status.h"
#include "gw_storage.h"
#include "gw_tagdb.h"

static const char *TAG = "main";

static gw_config_t s_cfg;   /* immutable after boot; shared read-only by every task */
static char s_hardware[64];

static void on_device_updated(uint16_t dev)
{
    gw_edge_notify_device(dev);
}

static void halt_with_config_error(const gw_cfg_err_t *err, const char *source)
{
    gw_status_set(GW_ST_CFG_ERROR, true);
    for (;;) {
        ESP_LOGE(TAG, "invalid configuration (%s): %s", source, err->msg);
        ESP_LOGE(TAG, "nothing is started; fix the JSON and reflash (see README)");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "other";
    }
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "%s %s (IDF %s), reset reason: %s", app->project_name, app->version,
             app->idf_ver, reset_reason());

    gw_port_init();
    gw_tagdb_reset();
    if (gw_status_led_start() != ESP_OK) {
        ESP_LOGE(TAG, "status LED unavailable");
    }
    gw_storage_init();  /* failure is logged; bdSeq then lives in RAM only */

    gw_cfg_err_t err;
    const char *source;
    if (!gw_storage_load_config(&s_cfg, &err, &source)) {
        halt_with_config_error(&err, source);
    }
    ESP_LOGI(TAG, "configuration from %s: group '%s', node '%s', %u devices, %u tags", source,
             s_cfg.sparkplug.group_id, s_cfg.sparkplug.edge_node_id, s_cfg.device_count,
             s_cfg.tag_count);
    if (s_cfg.sparkplug.primary_host_id[0] != '\0') {
        ESP_LOGW(TAG, "primary_host_id '%s' ignored: SPB-13 is not implemented in this build",
                 s_cfg.sparkplug.primary_host_id);
    }

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    snprintf(s_hardware, sizeof(s_hardware), "%s (ESP32-S3 rev %d.%d)", gw_bsp.name,
             chip.revision / 100, chip.revision % 100);

    ESP_ERROR_CHECK(gw_edge_start(&s_cfg, app->version, s_hardware));
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
}
