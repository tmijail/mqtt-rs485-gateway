#include "gw_bus.h"

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
    double value;
} write_req_t;

static gw_bus_callbacks_t s_cb;
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
    gw_write_status_t st = gw_poller_write(&s_poller, w->tag, w->value);
    s_cb.write_done(w->tag, st == GW_WRITE_OK);
}

static void bus_task(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    write_req_t w;
    for (;;) {
        esp_task_wdt_reset();
        while (xQueueReceive(s_writes, &w, 0) == pdTRUE) {
            do_write(&w);
        }
        uint32_t now = now_ms();
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

bool gw_bus_request_write(uint16_t tag, double value)
{
    const write_req_t w = {.tag = tag, .value = value};
    return xQueueSend(s_writes, &w, 0) == pdTRUE;
}
