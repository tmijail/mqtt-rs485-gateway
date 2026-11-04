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
#include "gw_mbap.h"
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
static TaskHandle_t s_task;
/* MB-15: one forwarded Modbus TCP transaction at a time (the TCP server's single slot) */
static QueueHandle_t s_fwd;
static StaticQueue_t s_fwd_buf;
static uint8_t s_fwd_storage[sizeof(gw_mbap_txn_t *)];
static SemaphoreHandle_t s_fwd_done;
static StaticSemaphore_t s_fwd_done_buf;
static atomic_bool s_fwd_busy;   /* set by the TCP server, cleared here once the txn ran */

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
    gw_write_status_t st = gw_poller_write_value(&s_poller, w->tag, &w->value);
    s_cb.write_done(w->tag, st == GW_WRITE_OK);
}

/* Pending DCMD writes (and the diagnostic read and quiesce requests that share their queue),
 * then a forwarded Modbus TCP request (MB-15), served before the next
 * cycle and between the requests of a cycle (MB-14): each waits at most for the transaction in
 * progress, not for a whole cycle. Priority: DCMD writes, then Modbus TCP, then polling. */
static void serve_pending(void *ctx)
{
    (void)ctx;
    if (atomic_load(&s_quiesce)) {
        stay_quiet();   /* also between two requests of a cycle: never returns */
    }
    write_req_t w;
    while (xQueueReceive(s_writes, &w, 0) == pdTRUE) {
        do_write(&w);
    }
    gw_mbap_txn_t *txn;
    if (xQueueReceive(s_fwd, &txn, 0) == pdTRUE) {
        gw_mbap_execute(&s_mb, txn);
        atomic_store(&s_fwd_busy, false);
        xSemaphoreGive(s_fwd_done);
    }
}

static void bus_task(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    for (;;) {
        esp_task_wdt_reset();
        serve_pending(NULL);
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
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait));   /* a write or a TCP request wakes it */
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
        .between_requests = serve_pending,
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
    s_fwd = xQueueCreateStatic(1, sizeof(gw_mbap_txn_t *), s_fwd_storage, &s_fwd_buf);
    s_fwd_done = xSemaphoreCreateBinaryStatic(&s_fwd_done_buf);
    s_task = xTaskCreateStaticPinnedToCore(bus_task, "rs485_bus", STACK_WORDS, NULL,
                                           CONFIG_GW_BUS_TASK_PRIORITY, s_stack, &s_task_buf,
                                           CONFIG_GW_BUS_TASK_CORE);
    return s_task != NULL ? ESP_OK : ESP_FAIL;
}

bool gw_bus_request_write(uint16_t tag, const gw_tag_value_t *value)
{
    const write_req_t w = {.tag = tag, .value = *value};
    if (xQueueSend(s_writes, &w, 0) != pdTRUE) {
        return false;
    }
    xTaskNotifyGive(s_task);
    return true;
}

size_t gw_bus_write_room(void)
{
    return (size_t)uxQueueSpacesAvailable(s_writes);
}

void gw_bus_set_scan_rate(uint32_t ms)
{
    atomic_store(&s_scan_rate, ms);   /* applied by the bus owner at the top of its loop */
}

bool gw_bus_forward_idle(uint32_t wait_ms)
{
    if (!atomic_load(&s_fwd_busy)) {
        return true;
    }
    xSemaphoreTake(s_fwd_done, pdMS_TO_TICKS(wait_ms));
    return !atomic_load(&s_fwd_busy);
}

bool gw_bus_forward(gw_mbap_txn_t *txn, uint32_t wait_ms)
{
    if (atomic_load(&s_fwd_busy)) {
        return false;
    }
    xSemaphoreTake(s_fwd_done, 0);   /* drop the give of a transaction that timed out */
    atomic_store(&s_fwd_busy, true);
    if (xQueueSend(s_fwd, &txn, 0) != pdTRUE) {
        atomic_store(&s_fwd_busy, false);
        return false;
    }
    xTaskNotifyGive(s_task);
    return xSemaphoreTake(s_fwd_done, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
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
    xTaskNotifyGive(s_task);   /* the idle bus task sleeps on its notification */
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
    xQueueSend(s_writes, &w, 0);   /* the flag covers a full queue */
    xTaskNotifyGive(s_task);       /* wakes an idle bus task */
    xSemaphoreTake(s_quiet, pdMS_TO_TICKS(timeout_ms));
}
