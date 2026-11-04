#include "gw_log.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_port.h"
#include "gw_syslog.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "syslog";

#define STACK_BYTES    3584
#define LINE_BYTES     256          /* one formatted esp_log line; longer ones are truncated */
#define OUT_BYTES      (GW_SYSLOG_MSG_MAX + 160)
#define RETRY_MS       5000
#define VALID_TIME_MS  1577836800000ULL   /* 2020-01-01: before that the clock is not set */
#define SEQ_MAX        2147483647u        /* RFC 5424 meta sequenceId range 1..2^31-1 */

static const gw_config_t *s_cfg;
static gw_syslog_ring_t s_ring;
static vprintf_like_t s_prev = vprintf;

/* producers (any task) */
static SemaphoreHandle_t s_fmt_lock;
static StaticSemaphore_t s_fmt_lock_buf;
static char s_line[LINE_BYTES];     /* guarded by s_fmt_lock */

/* sender task */
static TaskHandle_t s_task;
static StaticTask_t s_task_buf;
static StackType_t s_stack[STACK_BYTES / sizeof(StackType_t)];
static int s_sock = -1;
static TickType_t s_retry_at;
static uint32_t s_seq;
static char s_out[OUT_BYTES];

/* statistics (read by the shell) */
static atomic_bool s_enabled;
static atomic_bool s_net_up;        /* lwIP is running and an interface is active */
static atomic_bool s_connected;
static atomic_uint s_sent;
static atomic_uint s_dropped;
static atomic_uint s_errors;

static void capture(const char *fmt, va_list ap)
{
    /* cheap early filter on the level letter, before any formatting */
    int sev = gw_syslog_format_severity(fmt);
    if (sev > (int)s_cfg->syslog.level) {
        return;
    }
    /* Never wait: if another task is formatting right now, this line is not forwarded. */
    if (xSemaphoreTake(s_fmt_lock, 0) != pdTRUE) {
        gw_syslog_ring_count_drop(&s_ring);
        return;
    }
    bool queued = false;
    gw_syslog_rec_t *rec = gw_syslog_ring_reserve(&s_ring);   /* NULL (and counted) if full */
    if (rec != NULL) {
        vsnprintf(s_line, sizeof(s_line), fmt, ap);
        gw_syslog_parse_line(s_line, rec);
        if (rec->severity <= s_cfg->syslog.level) {
            uint64_t now = gw_port_time_ms();
            rec->time_ms = now >= VALID_TIME_MS ? now : 0;
            gw_syslog_ring_commit(&s_ring);
            queued = true;
        }
    }
    xSemaphoreGive(s_fmt_lock);
    if (queued) {
        xTaskNotifyGive(s_task);
    }
}

/* esp_log output hook: the console first, unchanged; then a copy for the syslog server. */
static int log_vprintf(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int r = s_prev(fmt, ap);
    if (!xPortInIsrContext() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
        xTaskGetCurrentTaskHandle() != s_task) {   /* the sender's own lines: no recursion */
        capture(fmt, copy);
    }
    va_end(copy);
    return r;
}

static bool open_socket(void)
{
    char port[8];
    snprintf(port, sizeof(port), "%u", s_cfg->syslog.port);
    const struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM};
    struct addrinfo *res = NULL;
    if (getaddrinfo(s_cfg->syslog.host, port, &hints, &res) != 0 || res == NULL) {
        return false;
    }
    int s = socket(res->ai_family, SOCK_DGRAM, IPPROTO_UDP);
    if (s >= 0 && connect(s, res->ai_addr, res->ai_addrlen) != 0) {
        close(s);
        s = -1;
    }
    freeaddrinfo(res);
    s_sock = s;
    return s >= 0;
}

static void close_socket(void)
{
    close(s_sock);
    s_sock = -1;
    atomic_store(&s_connected, false);
    s_retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(RETRY_MS);
}

static bool send_record(const gw_syslog_rec_t *rec)
{
    uint32_t seq = s_seq >= SEQ_MAX ? 1 : s_seq + 1;
    size_t n = gw_syslog_format(rec, s_cfg->sparkplug.edge_node_id, seq, s_out, sizeof(s_out));
    if (send(s_sock, s_out, n, 0) < 0) {
        int e = errno;
        atomic_fetch_add(&s_errors, 1);
        if (e != ENOMEM && e != EAGAIN && e != EWOULDBLOCK) {
            close_socket();   /* no route, interface gone...: resolve and reopen later */
        }
        return false;
    }
    s_seq = seq;
    atomic_fetch_add(&s_sent, 1);
    return true;
}

static void sender_task(void *arg)
{
    (void)arg;
    bool unreachable_logged = false;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        if (s_sock < 0) {
            if (!atomic_load(&s_net_up) || (int32_t)(xTaskGetTickCount() - s_retry_at) < 0) {
                continue;   /* lines stay in the ring until the network is up */
            }
            if (!open_socket()) {
                s_retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(RETRY_MS);
                if (!unreachable_logged) {   /* console only: this task is not forwarded */
                    ESP_LOGW(TAG, "%s:%u not reachable yet; up to %d lines are kept",
                             s_cfg->syslog.host, s_cfg->syslog.port, CONFIG_GW_SYSLOG_SLOTS - 1);
                    unreachable_logged = true;
                }
                continue;
            }
            unreachable_logged = false;
            atomic_store(&s_connected, true);
        }
        unsigned dropped = gw_syslog_ring_take_dropped(&s_ring);
        if (dropped != 0) {
            gw_syslog_rec_t note = {.severity = 4, .tag = "syslog"};
            uint64_t now = gw_port_time_ms();
            note.time_ms = now >= VALID_TIME_MS ? now : 0;
            snprintf(note.msg, sizeof(note.msg),
                     "%u log line(s) not forwarded (buffer full or logger busy)", dropped);
            if (send_record(&note)) {
                atomic_fetch_add(&s_dropped, dropped);
            } else {
                for (unsigned i = 0; i < dropped; i++) {   /* report them next time */
                    gw_syslog_ring_count_drop(&s_ring);
                }
                continue;
            }
        }
        const gw_syslog_rec_t *rec;
        while (s_sock >= 0 && (rec = gw_syslog_ring_peek(&s_ring)) != NULL) {
            if (!send_record(rec)) {
                break;   /* kept; retried on the next wake-up */
            }
            gw_syslog_ring_pop(&s_ring);
        }
    }
}

esp_err_t gw_log_start(const gw_config_t *cfg)
{
    if (cfg->syslog.host[0] == '\0') {
        return ESP_OK;
    }
    s_cfg = cfg;
    const size_t n = CONFIG_GW_SYSLOG_SLOTS;
    gw_syslog_rec_t *slots = heap_caps_calloc(n, sizeof(*slots), MALLOC_CAP_SPIRAM);
    if (slots == NULL) {
        slots = heap_caps_calloc(n, sizeof(*slots), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    ESP_RETURN_ON_FALSE(slots, ESP_ERR_NO_MEM, TAG, "no memory for %u lines", (unsigned)n);
    gw_syslog_ring_init(&s_ring, slots, (unsigned)n);
    s_fmt_lock = xSemaphoreCreateMutexStatic(&s_fmt_lock_buf);
    s_task = xTaskCreateStatic(sender_task, "syslog", STACK_BYTES / sizeof(StackType_t), NULL,
                               CONFIG_GW_SYSLOG_TASK_PRIORITY, s_stack, &s_task_buf);
    ESP_RETURN_ON_FALSE(s_task, ESP_FAIL, TAG, "task");
    atomic_store(&s_enabled, true);
    s_prev = esp_log_set_vprintf(log_vprintf);
    static const char *const names[8] = {[3] = "error", [4] = "warning", [6] = "info",
                                         [7] = "debug"};
    ESP_LOGI(TAG, "forwarding logs to %s:%u (RFC 5424 over UDP), level %s and above",
             cfg->syslog.host, cfg->syslog.port, names[cfg->syslog.level & 7]);
    return ESP_OK;
}

void gw_log_notify_network(bool up)
{
    atomic_store(&s_net_up, up);
    if (up && s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

void gw_log_get_stats(gw_log_stats_t *out)
{
    out->enabled = atomic_load(&s_enabled);
    out->connected = atomic_load(&s_connected);
    out->sent = atomic_load(&s_sent);
    out->dropped = atomic_load(&s_dropped) + atomic_load(&s_ring.dropped);
    out->send_errors = atomic_load(&s_errors);
}
