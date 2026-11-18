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
#include "gw_logring.h"
#include "gw_port.h"
#include "gw_storage.h"
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
static bool s_hooked;               /* log_vprintf is the esp_log output */
static atomic_bool s_syslog_on;     /* REL-4: the syslog copy runs */

/* #200 and #199: the live log of the web UI and the tags of the log level. The ring, the copy
 * buffer of the reader and the tag table are one allocation in PSRAM at boot (#135). */
static const char *const k_own_tags[] = {
    "ap",    "bus",   "button",  "buzzer", "coredump", "edge",  "fw",    "io",
    "led",   "main",  "mb",      "mbtcp",  "mdns",     "net",   "poll",  "rs485",
    "sfcard", "shell", "spb",    "storage", "syslog",  "web"};
static SemaphoreHandle_t s_lring_lock;   /* the ring and the tag table */
static StaticSemaphore_t s_lring_lock_buf;
static gw_logring_t s_lring;
static gw_logtags_t *s_tags;
static char *s_copy;                     /* the web server task reads the ring into it */
static size_t s_copy_size;
static atomic_uint s_lring_dropped;      /* lines that the ring did not get (another task held it) */

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

/* #200: the line into the ring of the live log, and its tag into the tag table (#199). Never
 * waits: a line that comes while a reader copies the ring is counted, not kept. */
static void ring_put(const char *line)
{
    if (xSemaphoreTake(s_lring_lock, 0) != pdTRUE) {
        atomic_fetch_add(&s_lring_dropped, 1);
        return;
    }
    size_t len = strlen(line);
    gw_logring_put(&s_lring, line, len);
    const char *tag;
    size_t tag_len;
    if (gw_logring_tag(line, len, &tag, &tag_len)) {
        gw_logtags_add(s_tags, tag, tag_len);
    }
    xSemaphoreGive(s_lring_lock);
}

static void capture(const char *fmt, va_list ap, bool syslog)
{
    bool ring = s_tags != NULL;
    /* cheap early filter on the level letter, before any formatting */
    syslog = syslog && atomic_load(&s_syslog_on) &&
             gw_syslog_format_severity(fmt) <= (int)s_cfg->syslog.level;
    if (!ring && !syslog) {
        return;
    }
    /* Never wait: if another task is formatting right now, this line is not copied. */
    if (xSemaphoreTake(s_fmt_lock, 0) != pdTRUE) {
        if (ring) {
            atomic_fetch_add(&s_lring_dropped, 1);
        }
        if (syslog) {
            gw_syslog_ring_count_drop(&s_ring);
        }
        return;
    }
    vsnprintf(s_line, sizeof(s_line), fmt, ap);
    if (ring) {
        ring_put(s_line);
    }
    bool queued = false;
    gw_syslog_rec_t *rec = syslog ? gw_syslog_ring_reserve(&s_ring) : NULL;   /* NULL if full */
    if (rec != NULL) {
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

/* esp_log output hook: the console first, unchanged; then a copy for the live log and the syslog
 * server. */
static int log_vprintf(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int r = s_prev(fmt, ap);
    if (!xPortInIsrContext() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        /* the syslog sender's own lines go to the live log only: no recursion */
        capture(fmt, copy, s_task == NULL || xTaskGetCurrentTaskHandle() != s_task);
    }
    va_end(copy);
    return r;
}

static void hook(void)
{
    if (!s_hooked) {
        s_fmt_lock = xSemaphoreCreateMutexStatic(&s_fmt_lock_buf);
        s_hooked = true;
        s_prev = esp_log_set_vprintf(log_vprintf);
    }
}

esp_err_t gw_log_ring_start(void)
{
    const size_t size = CONFIG_GW_LOG_RING_KB * 1024u;
    /* the tag table, the ring and the copy buffer of the reader: one allocation (PSRAM). The
     * table is first, at the alignment of the allocation (#227, -Wcast-align). */
    gw_logtags_t *tags = gw_storage_malloc(sizeof(gw_logtags_t) + 2 * size);
    ESP_RETURN_ON_FALSE(tags, ESP_ERR_NO_MEM, TAG, "no memory for the live log (%u KB)",
                        (unsigned)(2 * size / 1024));
    char *mem = (char *)(tags + 1);
    s_lring_lock = xSemaphoreCreateMutexStatic(&s_lring_lock_buf);
    gw_logring_init(&s_lring, mem, size);
    s_copy = mem + size;
    s_copy_size = size;
    gw_logtags_init(tags);
    for (size_t i = 0; i < sizeof(k_own_tags) / sizeof(k_own_tags[0]); i++) {
        gw_logtags_add(tags, k_own_tags[i], strlen(k_own_tags[i]));
    }
    s_tags = tags;   /* last: the hook copies lines from here on */
    hook();
    return ESP_OK;
}

bool gw_log_ring_copy(uint32_t since, const char **text, size_t *len, gw_log_ring_info_t *info)
{
    if (s_tags == NULL || xSemaphoreTake(s_lring_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return false;
    }
    *len = gw_logring_copy(&s_lring, since, s_copy, s_copy_size, &info->from, &info->last);
    info->first = gw_logring_first(&s_lring);
    info->next = s_lring.next;
    info->lines = s_lring.lines;
    info->used = s_lring.used;
    info->size = s_lring.size;
    xSemaphoreGive(s_lring_lock);
    info->dropped = atomic_load(&s_lring_dropped);
    *text = s_copy;
    return true;
}

size_t gw_log_tags(gw_logtag_t *out, size_t cap)
{
    if (s_tags == NULL || xSemaphoreTake(s_lring_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return 0;
    }
    size_t n = s_tags->count < cap ? s_tags->count : cap;
    memcpy(out, s_tags->tag, n * sizeof(out[0]));
    xSemaphoreGive(s_lring_lock);
    return n;
}

int gw_log_build_level(void)
{
    return CONFIG_LOG_DEFAULT_LEVEL;
}

int gw_log_max_level(void)
{
    return CONFIG_LOG_MAXIMUM_LEVEL;
}

size_t gw_log_tags_json(bool changed_only, char *out, size_t cap)
{
    if (s_tags == NULL || xSemaphoreTake(s_lring_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        snprintf(out, cap, "[]");
        return cap > 2 ? 2 : 0;
    }
    size_t n = gw_logtags_json(s_tags, changed_only, out, cap);
    xSemaphoreGive(s_lring_lock);
    return n;
}

gw_log_set_t gw_log_level_set(const char *tag, int level)
{
    if (!gw_logtag_valid(tag)) {
        return GW_LOG_SET_BAD_TAG;
    }
    if (level < GW_LOGLV_NONE || level > gw_log_max_level()) {
        return GW_LOG_SET_BAD_LEVEL;
    }
    if (s_tags == NULL || xSemaphoreTake(s_lring_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return GW_LOG_SET_NO_ROOM;
    }
    bool ok = gw_logtags_set(s_tags, tag, level, gw_log_build_level());
    xSemaphoreGive(s_lring_lock);
    if (!ok) {
        return GW_LOG_SET_NO_ROOM;
    }
    esp_log_level_set(tag, (esp_log_level_t)level);
    ESP_LOGW(TAG, "log level of '%s': %s (not saved: a restart sets the level of the build again)",
             tag, gw_loglevel_name(level));
    return GW_LOG_SET_OK;
}

void gw_log_level_reset(void)
{
    if (s_tags == NULL || xSemaphoreTake(s_lring_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return;
    }
    /* esp_log keeps the level of each tag that it set: each changed tag gets the level of the
     * build again, "*" last */
    const int def = gw_log_build_level();
    for (unsigned i = 0; i < s_tags->count; i++) {
        const gw_logtag_t *t = &s_tags->tag[i];
        if (t->level != GW_LOGTAG_DEFAULT && strcmp(t->name, "*") != 0) {
            esp_log_level_set(t->name, (esp_log_level_t)def);
        }
    }
    esp_log_level_set("*", (esp_log_level_t)def);
    gw_logtags_reset_all(s_tags);
    xSemaphoreGive(s_lring_lock);
    ESP_LOGW(TAG, "log levels: all the tags have the level of the build again (%s)",
             gw_loglevel_name(def));
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
    gw_syslog_rec_t *slots = gw_storage_calloc(n, sizeof(*slots));   /* PSRAM (#135) */
    ESP_RETURN_ON_FALSE(slots, ESP_ERR_NO_MEM, TAG, "no memory for %u lines", (unsigned)n);
    gw_syslog_ring_init(&s_ring, slots, (unsigned)n);
    s_task = xTaskCreateStatic(sender_task, "syslog", STACK_BYTES / sizeof(StackType_t), NULL,
                               CONFIG_GW_SYSLOG_TASK_PRIORITY, s_stack, &s_task_buf);
    ESP_RETURN_ON_FALSE(s_task, ESP_FAIL, TAG, "task");
    atomic_store(&s_enabled, true);
    hook();   /* already done by gw_log_ring_start(), unless it had no memory */
    atomic_store(&s_syslog_on, true);
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
