#include "gw_bus.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
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

/* CFG-6 diagnostic read: one request at a time, handed to the bus task through the write
 * queue (tag DIAG_TAG) so it is served as promptly as a DCMD write. */
#define DIAG_TAG UINT16_MAX
#define QUIESCE_TAG (UINT16_MAX - 1)
static atomic_bool s_quiesce;
static SemaphoreHandle_t s_quiet;
static StaticSemaphore_t s_quiet_buf;
static struct {
    mb_request_t rq;
    mb_status_t status;
    uint8_t exception;
    uint16_t values[GW_BUS_DIAG_MAX];
} s_diag;
static atomic_bool s_diag_busy;
static SemaphoreHandle_t s_diag_done;
static StaticSemaphore_t s_diag_done_buf;

static void do_diag(void)
{
    mb_response_t rsp;
    s_diag.status = mb_master_execute(&s_mb, &s_diag.rq, &rsp);
    s_diag.exception = s_diag.status == MB_ERR_EXCEPTION ? rsp.exception : 0;
    bool bits = s_diag.rq.fc == MB_FC_READ_COILS || s_diag.rq.fc == MB_FC_READ_DISCRETE_INPUTS;
    for (uint16_t i = 0; s_diag.status == MB_OK && i < s_diag.rq.qty; i++) {
        s_diag.values[i] = bits ? (uint16_t)mb_rsp_bit(&rsp, i) : mb_rsp_reg(&rsp, i);
    }
    ESP_LOGI(TAG, "diagnostic read unit %u FC%02u %u x%u: status %d", s_diag.rq.unit,
             (unsigned)s_diag.rq.fc, s_diag.rq.addr, s_diag.rq.qty, (int)s_diag.status);
    atomic_store(&s_diag_busy, false);
    xSemaphoreGive(s_diag_done);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Restart pending: no more bus traffic; keep feeding the watchdog until it happens. */
static void stay_quiet(void)
{
    ESP_LOGI(TAG, "bus quiet for the restart");
    xSemaphoreGive(s_quiet);
    for (;;) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void do_write(const write_req_t *w)
{
    if (w->tag == QUIESCE_TAG) {
        stay_quiet();
    }
    if (w->tag == DIAG_TAG) {
        do_diag();
        return;
    }
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
        if (atomic_load(&s_quiesce)) {
            stay_quiet();
        }
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

    s_diag_done = xSemaphoreCreateBinaryStatic(&s_diag_done_buf);
    s_quiet = xSemaphoreCreateBinaryStatic(&s_quiet_buf);
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

mb_status_t gw_bus_diag_read(uint8_t unit, mb_fc_t fc, uint16_t addr, uint16_t count,
                             uint16_t out[GW_BUS_DIAG_MAX], uint8_t *exception,
                             uint32_t timeout_ms)
{
    *exception = 0;
    if (s_writes == NULL || count == 0 || count > GW_BUS_DIAG_MAX || fc < MB_FC_READ_COILS ||
        fc > MB_FC_READ_INPUT_REGISTERS || atomic_exchange(&s_diag_busy, true)) {
        return MB_ERR_ARG;
    }
    xSemaphoreTake(s_diag_done, 0);   /* a give left by an abandoned request */
    s_diag.rq = (mb_request_t){.unit = unit, .fc = fc, .addr = addr, .qty = count};
    const write_req_t w = {.tag = DIAG_TAG};
    if (xQueueSend(s_writes, &w, 0) != pdTRUE) {
        atomic_store(&s_diag_busy, false);
        return MB_ERR_ARG;
    }
    if (xSemaphoreTake(s_diag_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return MB_ERR_ARG;   /* stays busy until the bus task answers */
    }
    memcpy(out, s_diag.values, count * sizeof(uint16_t));
    *exception = s_diag.exception;
    return s_diag.status;
}

void gw_bus_quiesce(uint32_t timeout_ms)
{
    if (s_writes == NULL || atomic_exchange(&s_quiesce, true)) {
        return;
    }
    const write_req_t w = {.tag = QUIESCE_TAG};
    xQueueSend(s_writes, &w, 0);   /* wakes an idle bus task; the flag covers a full queue */
    xSemaphoreTake(s_quiet, pdMS_TO_TICKS(timeout_ms));
}
