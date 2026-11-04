#include "gw_bus.h"

#include <stdatomic.h>

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gw_poll.h"
#include "gw_status.h"

static const char *TAG = "bus";

#define STACK_WORDS (4096 / sizeof(StackType_t))
#define MAX_IDLE_MS 1000u   /* wake at least this often to feed the watchdog */

typedef struct {
    uint16_t tag;
    gw_tag_value_t value;   /* member per gw_conv_kind (MB-6: exact 64-bit integers, strings) */
} write_req_t;

static gw_bus_callbacks_t s_cb;
static _Atomic uint32_t s_scan_rate;          /* SPB-15: latest request from the Sparkplug task */
static uint32_t s_scan_rate_applied;
static mb_master_t s_mb;
static gw_poller_t s_poller;
static QueueHandle_t s_writes;
static StaticQueue_t s_writes_buf;
static uint8_t s_writes_storage[CONFIG_GW_BUS_WRITE_QUEUE_LEN * sizeof(write_req_t)];
static StaticTask_t s_task_buf;
static StackType_t s_stack[STACK_WORDS];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void do_write(const write_req_t *w)
{
    gw_write_status_t st = gw_poller_write_value(&s_poller, w->tag, &w->value);
    s_cb.write_done(w->tag, st == GW_WRITE_OK);
}

/* Pending DCMD writes, served before the next cycle and between the requests of a cycle
 * (MB-14): a write waits at most for the transaction in progress, not for a whole cycle. */
static void serve_writes(void *ctx)
{
    (void)ctx;
    write_req_t w;
    while (xQueueReceive(s_writes, &w, 0) == pdTRUE) {
        do_write(&w);
    }
}

static void bus_task(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    write_req_t w;
    for (;;) {
        esp_task_wdt_reset();
        serve_writes(NULL);
        uint32_t now = now_ms();
        uint32_t rate = atomic_load(&s_scan_rate);
        if (rate != s_scan_rate_applied) {
            s_scan_rate_applied = rate;
            gw_poller_set_scan_rate(&s_poller, rate, now);
            ESP_LOGI(TAG, "scan rate %lu ms%s", (unsigned long)rate,
                     rate == 0 ? ": each device at its poll_ms" : " for every device");
        }
        uint32_t wait;
        int dev = gw_poller_next(&s_poller, now, &wait);
        if (dev >= 0) {
            gw_poller_poll(&s_poller, (uint16_t)dev, now);
            s_cb.device_updated((uint16_t)dev);
            gw_status_set(GW_ST_MB_FAULT, gw_poller_offline_count(&s_poller) > 0);
            continue;
        }
        if (wait > MAX_IDLE_MS) {
            wait = MAX_IDLE_MS;
        }
        if (xQueueReceive(s_writes, &w, pdMS_TO_TICKS(wait)) == pdTRUE) {
            do_write(&w);
        }
    }
}

esp_err_t gw_bus_start(const gw_config_t *cfg, const mb_transport_t *tr, const gw_bus_callbacks_t *cb)
{
    s_cb = *cb;
    const mb_master_cfg_t mc = {
        .baud = cfg->rs485.baud,
        .response_timeout_ms = cfg->rs485.response_timeout_ms,
        .retries = cfg->rs485.retries,
        .inter_frame_delay_ms = cfg->rs485.inter_frame_delay_ms,
    };
    mb_master_init(&s_mb, tr, &mc);
    const gw_poller_opts_t opts = {
        .offline_probe_max_ms = CONFIG_GW_MB_OFFLINE_PROBE_MAX_MS,
        .between_requests = serve_writes,
#if CONFIG_GW_MB_BLOCK_READS
        .merge_reads = true,
        .max_gap = CONFIG_GW_MB_BLOCK_MAX_GAP,
#endif
#if CONFIG_GW_MB_FORCE_MULTI_WRITE
        .force_multi_write = true,
#endif
    };
    gw_poller_init(&s_poller, cfg, &s_mb, &opts, now_ms());
    ESP_LOGI(TAG, "%u devices, %u tags, %u read requests per full cycle", cfg->device_count,
             cfg->tag_count, s_poller.plan.block_count);
    gw_status_set(GW_ST_MB_FAULT, cfg->device_count > 0);  /* until the first answers */

    s_writes = xQueueCreateStatic(CONFIG_GW_BUS_WRITE_QUEUE_LEN, sizeof(write_req_t),
                                  s_writes_storage, &s_writes_buf);
    TaskHandle_t t = xTaskCreateStaticPinnedToCore(bus_task, "rs485_bus", STACK_WORDS, NULL,
                                                   CONFIG_GW_BUS_TASK_PRIORITY, s_stack, &s_task_buf,
                                                   CONFIG_GW_BUS_TASK_CORE);
    return t != NULL ? ESP_OK : ESP_FAIL;
}

bool gw_bus_request_write(uint16_t tag, const gw_tag_value_t *value)
{
    const write_req_t w = {.tag = tag, .value = *value};
    return xQueueSend(s_writes, &w, 0) == pdTRUE;
}

size_t gw_bus_write_room(void)
{
    return (size_t)uxQueueSpacesAvailable(s_writes);
}

void gw_bus_set_scan_rate(uint32_t ms)
{
    atomic_store(&s_scan_rate, ms);   /* applied by the bus owner at the top of its loop */
}
