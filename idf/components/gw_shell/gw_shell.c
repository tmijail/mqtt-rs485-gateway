#include "gw_shell.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_ipc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_bootbtn.h"
#include "gw_bus.h"
#include "gw_cfgdoc.h"
#include "gw_edge.h"
#include "gw_fw.h"
#include "gw_io.h"
#include "gw_log.h"
#include "gw_mbap.h"
#include "gw_net.h"
#include "gw_rtc.h"
#include "gw_status.h"
#include "gw_storage.h"
#include "gw_tagdb.h"
#include "gw_web.h"
#include "lwip/ip_addr.h"
#include "ping/ping_sock.h"

static const char *TAG = "shell";

static gw_shell_ctx_t s_ctx;
static SemaphoreHandle_t s_ctx_lock;   /* a command runs with the context it started with */
static StaticSemaphore_t s_ctx_lock_buf;
static volatile unsigned s_up;         /* GW_SHELL_UP_* */

void gw_shell_mark_up(unsigned what)
{
    __atomic_fetch_or(&s_up, what, __ATOMIC_SEQ_CST);
}

void gw_shell_set_ctx(const gw_shell_ctx_t *ctx)
{
    xSemaphoreTake(s_ctx_lock, portMAX_DELAY);
    s_ctx = *ctx;
    xSemaphoreGive(s_ctx_lock);
}
static gw_tag_value_t s_vals[GW_MAX_TAGS_PER_DEVICE];   /* scratch for gw_tagdb_get_device */

/* ---------- answers atomic with respect to the log (#56) ----------
 * The console UART carries the log of every task and the shell's answers. A command builds its
 * answer in s_out and writes it with one fwrite under s_console, which the log output also
 * takes (log_vprintf, installed as esp_log's vprintf): a log line can come before or after an
 * answer, never inside one of its lines. Long answers go out in pieces of whole lines. */
static SemaphoreHandle_t s_console;
static StaticSemaphore_t s_console_buf;
static vprintf_like_t s_prev_vprintf;
static char s_out[1024];
static size_t s_out_len;

#define CONSOLE_LOCK_MS 500   /* a log line never waits longer than this for an answer */

static int log_vprintf(const char *fmt, va_list ap)
{
    bool locked = !xPortInIsrContext() &&
                  xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
                  xSemaphoreTakeRecursive(s_console, pdMS_TO_TICKS(CONSOLE_LOCK_MS)) == pdTRUE;
    int r = s_prev_vprintf(fmt, ap);
    if (locked) {
        xSemaphoreGiveRecursive(s_console);
    }
    return r;
}

static void write_locked(const char *data, size_t len)
{
    xSemaphoreTakeRecursive(s_console, portMAX_DELAY);
    fwrite(data, 1, len, stdout);
    fflush(stdout);
    xSemaphoreGiveRecursive(s_console);
}

static void out_flush(void)
{
    if (s_out_len > 0) {
        write_locked(s_out, s_out_len);
        s_out_len = 0;
    }
}

/* Make room: write the complete lines buffered so far, keep a started line. */
static void out_make_room(void)
{
    size_t keep = 0;
    while (keep < s_out_len && s_out[s_out_len - 1 - keep] != '\n') {
        keep++;
    }
    if (keep == s_out_len) {   /* a single line longer than the buffer: write it as it is */
        out_flush();
        return;
    }
    write_locked(s_out, s_out_len - keep);
    memmove(s_out, s_out + s_out_len - keep, keep);
    s_out_len = keep;
}

static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void out(const char *fmt, ...)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(s_out + s_out_len, sizeof(s_out) - s_out_len, fmt, ap);
        va_end(ap);
        if (n >= 0 && (size_t)n < sizeof(s_out) - s_out_len) {
            s_out_len += (size_t)n;
            return;
        }
        if (attempt == 0) {
            out_make_room();
        } else {
            s_out_len = sizeof(s_out) - 1;   /* truncated */
        }
    }
}

/* Large text (config show): copied in pieces through the buffer. */
static void out_raw(const char *data, size_t len)
{
    while (len > 0) {
        if (s_out_len == sizeof(s_out)) {
            out_make_room();
        }
        size_t n = sizeof(s_out) - s_out_len;
        n = n < len ? n : len;
        memcpy(s_out + s_out_len, data, n);
        s_out_len += n;
        data += n;
        len -= n;
    }
}

/* Every command runs through here: its answer leaves in one piece when it returns. */
/* Issue #72: commands typed while the gateway starts are answered, not lost: one whose
 * subsystem is not running yet says so. */
static bool up(unsigned what, const char *cmd, const char *name)
{
    if ((__atomic_load_n(&s_up, __ATOMIC_SEQ_CST) & what) == what) {
        return true;
    }
    out("%s: not available yet: %s is starting\n", cmd, name);
    return false;
}

#define ATOMIC(fn)                                    \
    static int fn##_atomic(int argc, char **argv)     \
    {                                                 \
        xSemaphoreTake(s_ctx_lock, portMAX_DELAY);    \
        s_out_len = 0;                                \
        int rc = fn(argc, argv);                      \
        out_flush();                                  \
        xSemaphoreGive(s_ctx_lock);                   \
        return rc;                                    \
    }

static bool device_online(uint16_t dev)
{
    bool online;
    gw_tagdb_get_device(s_ctx.cfg, dev, s_vals, &online);
    return online;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint32_t f = gw_status_get();
    out("firmware %s, reset reason %s, uptime %lld s\n", s_ctx.info->firmware,
           s_ctx.info->reset_reason, esp_timer_get_time() / 1000000);
    /* FW-1: from the heap for the command (not the console task's stack nor .bss, #80) */
    gw_fw_status_t *fw = heap_caps_malloc_prefer(sizeof(*fw), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                                 MALLOC_CAP_8BIT);
    if (fw != NULL) {
        gw_fw_get_status(fw);
    }
    if (fw != NULL && fw->running[0] != '\0') {
        out("ota      %s in %s, other slot %s%s | %s", fw->running, fw->slot,
            fw->other[0] ? fw->other : "empty", fw->enabled ? "" : " (updates disabled)",
            fw->detail);
        if (fw->phase == GW_FW_TRIAL && fw->trial_left_s > 0) {   /* 0: confirmation retried */
            out(" (%lu s left)", (unsigned long)fw->trial_left_s);
        } else if (fw->phase == GW_FW_DOWNLOADING) {
            out(" %u%% (%lu of %lu bytes)", fw->progress, (unsigned long)fw->bytes,
                (unsigned long)fw->total);
        }
        out("\n");
    }
    heap_caps_free(fw);
    if (s_ctx.cfg == NULL && s_ctx.starting) {
        out("config   loading: the gateway is starting\n");
    } else if (s_ctx.cfg == NULL && s_ctx.commissioning) {
        out("config   NONE: commissioning after a factory reset; load one with the web UI or "
               "'config load'\n");
        out("         nothing is running: no polling, no MQTT; network and web UI only\n");
        out("network  %s\n", (f & GW_ST_NET_UP) ? "up" : "down");
    } else if (s_ctx.cfg == NULL) {
        out("config   REJECTED (%s): %s\n", s_ctx.config_source, s_ctx.config_error);
        out("         nothing is running: no polling, no MQTT; recovery network and web UI "
               "only\n");
        out("network  %s\n", (f & GW_ST_NET_UP) ? "up" : "down");
    } else {
        uint16_t offline = 0;
        for (uint16_t d = 0; d < s_ctx.cfg->device_count; d++) {
            offline = (uint16_t)(offline + (device_online(d) ? 0u : 1u));
        }
        out("config   %s: ok%s\n", s_ctx.config_source,
            s_ctx.starting ? " (the gateway is still starting)" : "");
        out("network  %s, MQTT %s, Modbus %u/%u devices online\n",
               (f & GW_ST_NET_UP) ? "up" : "down",
               (f & GW_ST_MQTT_UP) ? "connected" : "disconnected",
               s_ctx.cfg->device_count - offline, s_ctx.cfg->device_count);
        gw_edge_status_t es;
        gw_edge_get_status(&es);
        if (es.host != NULL) {
            out("mqtt     broker %u/%u %s:%u%s, %u failed attempt(s) in a row\n", es.broker + 1u,
                es.broker_count, es.host, es.port, s_ctx.cfg->mqtt.tls.enabled ? " (TLS)" : "",
                es.failures);
        }
        if (es.max_packet != 0 || es.oversize != 0) {
            out("limit    broker Maximum Packet Size %lu B; %lu message(s) too large, not "
                "published%s%s%s\n",
                (unsigned long)es.max_packet, (unsigned long)es.oversize,
                es.oversize ? " (last: " : "", es.oversize ? es.oversize_topic : "",
                es.oversize ? ")" : "");
        }
        if (es.sf_enabled) {
            out("store    %lu of %lu samples waiting (%s), %lu historical messages sent, %lu "
                "overwritten\n",
                (unsigned long)es.sf_count, (unsigned long)es.sf_cap,
                es.sf_psram ? "PSRAM" : "internal RAM", (unsigned long)es.sf_sent,
                (unsigned long)es.sf_dropped);
            if (es.spill_on) {
                out("spill    %s: %lu samples on the card, %lu moved there, %lu deleted unsent\n",
                    es.spill_failed ? "FAILED (the ring overwrites)" : "on", (unsigned long)es.spill_count,
                    (unsigned long)es.spilled, (unsigned long)es.spill_dropped);
            }
        }
        if (s_ctx.cfg->sparkplug.primary_host_id[0] != '\0') {
            out("host     primary '%s' %s%s\n", s_ctx.cfg->sparkplug.primary_host_id,
                es.host_state < 0 ? "not seen yet" : es.host_state ? "online" : "offline",
                es.waiting_host ? ", NBIRTH waits for it" : "");
        }
    }
    gw_net_if_status_t ifs[GW_NET_IF_COUNT];
    gw_net_get_status(ifs);
    static const char *const names[GW_NET_IF_COUNT] = {"ethernet", "wifi"};
    out("iface    ");
    for (int i = 0; i < GW_NET_IF_COUNT; i++) {
        const gw_net_if_status_t *n = &ifs[i];
        out("%s%s", i ? " | " : "", names[i]);
        if (!n->configured) {
            out(" not configured");
            continue;
        }
        out(" %s", !n->link ? "link down" : n->usable ? "up" : "link up, no address");
        if (n->usable) {
            out(" %u.%u.%u.%u", n->ipv4[0], n->ipv4[1], n->ipv4[2], n->ipv4[3]);
        }
        if (n->active) {
            out(" (active)");
        }
    }
    out("\n");
    bool any6 = false;
    for (int i = 0; i < GW_NET_IF_COUNT; i++) {
        for (int k = 0; k < GW_NET_IPV6_MAX; k++) {
            if (ifs[i].ipv6[k][0] != '\0') {
                out("%s%s %s", any6 ? ", " : "ipv6     ", names[i], ifs[i].ipv6[k]);
                any6 = true;
            }
        }
    }
    if (any6) {
        out("\n");
    }
    gw_web_stats_t ws;
    gw_web_get_stats(&ws);
    if (ws.running) {
        out("web      %s, logins failed %lu, refused while blocked %lu, blocks %lu, addresses "
            "blocked now %u, cross-site writes refused %lu, bodies cut %lu\n",
            ws.https ? "HTTPS (HTTP redirects)" : "HTTP", (unsigned long)ws.failed,
            (unsigned long)ws.refused, (unsigned long)ws.blocks, ws.blocked_now,
            (unsigned long)ws.csrf_refused, (unsigned long)ws.cut_bodies);
    }
    gw_log_stats_t ls;
    gw_log_get_stats(&ls);
    if (ls.enabled && s_ctx.cfg != NULL) {
        out("syslog   %s:%u %s, sent %lu, dropped %lu, errors %lu\n", s_ctx.cfg->syslog.host,
               s_ctx.cfg->syslog.port, ls.connected ? "connected" : "not reachable",
               (unsigned long)ls.sent, (unsigned long)ls.dropped, (unsigned long)ls.send_errors);
    }
#if CONFIG_NVS_ENCRYPTION && CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC
    const char *no_nvs = gw_storage_nvs_unavailable_why();
    if (no_nvs != NULL) {   /* issue #94 */
        out("secrets  NVS UNAVAILABLE this boot: %s\n", no_nvs);
    } else if (gw_storage_nvs_encryption_pending()) {   /* issue #94 */
        out("secrets  NVS NOT encrypted yet (new firmware on trial, or no LittleFS for the "
            "migration): encrypted at the next boot of a confirmed firmware\n");
    } else {
        out("secrets  NVS %s (HMAC key in eFuse KEY%d)\n",
            gw_storage_secrets_encrypted() ? "encrypted" : "UNAVAILABLE",
            CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID);
    }
#else
    out("secrets  NVS NOT encrypted (default build; sdkconfig.production encrypts it)\n");
#endif
    gw_local_io_t io;
    gw_tagdb_get_io(&io);
    gw_io_expander_t exp;
    gw_io_get_expander(&exp);
    if (io.do_unknown) {
        out("local    DI 0x%02x, DO unknown: output expander not answering (%s, %u attempt(s))\n",
            io.di, exp.retrying ? "retrying" : "not retried", exp.attempts);
    } else {
        out("local    DI 0x%02x, DO 0x%02x (bit n = channel n+1, 1 = active)%s\n", io.di, io.dout,
            io.failsafe ? ", failsafe: safe state applied (no Sparkplug session)" : "");
    }
    /* TIME-1/TIME-2: the clock and where it came from */
    gw_io_rtc_info_t rtc;
    gw_io_rtc_get(&rtc);
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    bool synced = gw_edge_time_synced();
    out("time     %04d-%02d-%02dT%02d:%02d:%02dZ, %s; RTC ", tm.tm_year + 1900, tm.tm_mon + 1,
        tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
        synced ? "SNTP synchronised" : rtc.restored ? "from the RTC (not synchronised)"
                                                    : "not set (no SNTP yet)");
    if (!rtc.present) {
        out("not answering\n");
    } else {
        out("%s, stored %lu time(s)%s\n", gw_rtc_status_str((gw_rtc_status_t)rtc.status),
            (unsigned long)rtc.stores, rtc.store_errors ? ", write errors" : "");
    }
    out("counters");   /* IO-4: activations of each DI */
    for (int i = 0; i < 8; i++) {
        out(" %d:%" PRIu64, i + 1, io.count[i]);
    }
    out("\n");
    /* Core of each task that matters for timing ("-" = no affinity; tasks not running, such as
     * the Ethernet RX task of the other MAC, are omitted). */
    static const char *const tasks[] = {"rs485_bus", "sparkplug", "tcpip", "mqtt_task",
                                        "w5500_tsk", "emac_rx",   "wifi",      "sys_evt"};
    out("tasks   ");
    for (size_t i = 0; i < sizeof(tasks) / sizeof(tasks[0]); i++) {
        TaskHandle_t h = xTaskGetHandle(tasks[i]);
        if (h == NULL) {
            continue;
        }
        BaseType_t core = xTaskGetCoreID(h);
        if (core == tskNO_AFFINITY) {
            out(" %s -", tasks[i]);
        } else {
            out(" %s %d", tasks[i], (int)core);
        }
    }
    out(" (core)\n");
    /* Lowest free stack since boot of the tasks of ours whose depth depends on the traffic
     * (Sparkplug: births, nested template/DataSet coding, history replay; Modbus) */
    static const char *const deep[] = {"sparkplug", "mqtt_task", "rs485_bus", "local_io", "httpd"};
    out("stack   ");
    for (size_t i = 0; i < sizeof(deep) / sizeof(deep[0]); i++) {
        TaskHandle_t h = xTaskGetHandle(deep[i]);
        if (h != NULL) {
            out(" %s %u", deep[i], (unsigned)uxTaskGetStackHighWaterMark(h));
        }
    }
    out(" (B free, high-water mark)\n");
    /* issue #80: the minimum since boot, and the largest block (now and the lowest seen when a
     * configuration was validated, the deepest point of the worst case without PSRAM) */
    out("heap     internal %u B free (minimum %u B, largest block %u B, lowest at a validation "
        "%u B), PSRAM %u B free\n",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)gw_storage_validation_min_block(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return 0;
}

static int cmd_devices(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (s_ctx.cfg == NULL) {
        out(s_ctx.starting ? "devices: not available yet: the configuration is loading\n"
                           : "no configuration\n");
        return 1;
    }
    for (uint16_t d = 0; d < s_ctx.cfg->device_count; d++) {
        const gw_device_cfg_t *dev = &s_ctx.cfg->devices[d];
        gw_dev_stats_t st;
        gw_tagdb_get_stats(d, &st);
        out("%-16s unit %3u %-7s cycles %lu failed %lu | ok %lu timeout %lu crc %lu frame %lu "
               "exception %lu",
               dev->name, dev->unit_id, device_online(d) ? "online" : "offline",
               (unsigned long)st.cycles, (unsigned long)st.failed_cycles, (unsigned long)st.bus.ok,
               (unsigned long)st.bus.timeout, (unsigned long)st.bus.crc,
               (unsigned long)st.bus.frame, (unsigned long)st.bus.exception);
        const mb_stats_t *t = &st.tcp;   /* #69: Modbus TCP requests forwarded to this unit */
        if (t->ok + t->timeout + t->crc + t->frame + t->exception > 0) {
            out(" | tcp ok %lu timeout %lu crc %lu frame %lu exception %lu", (unsigned long)t->ok,
                (unsigned long)t->timeout, (unsigned long)t->crc, (unsigned long)t->frame,
                (unsigned long)t->exception);
        }
        out("\n");
    }
    if (s_ctx.cfg->modbus_tcp.enabled) {
        gw_mbap_stats_t g;
        gw_mbap_stats_get(&g);
        const mb_stats_t *o = &g.other_units;
        out("modbus_tcp connections %lu accepted %lu rejected | forwarded %lu | gateway "
            "exceptions 01 %lu 03 %lu 0A %lu 0B %lu | other units ok %lu timeout %lu crc %lu "
            "frame %lu exception %lu\n",
            (unsigned long)g.accepted, (unsigned long)g.rejected, (unsigned long)g.forwarded,
            (unsigned long)g.ex_function, (unsigned long)g.ex_value, (unsigned long)g.ex_path,
            (unsigned long)g.ex_no_answer, (unsigned long)o->ok, (unsigned long)o->timeout,
            (unsigned long)o->crc, (unsigned long)o->frame, (unsigned long)o->exception);
    }
    return 0;
}

static int config_show(void)
{
    size_t len;
    gw_cfg_err_t err;
    char *doc = gw_storage_export_config(&len, &err);
    if (doc == NULL) {
        out("cannot export: %s\n", err.msg);
        return 1;
    }
    out_raw(doc, len);
    gw_storage_free(doc);
    return 0;
}

#if CONFIG_GW_SHELL_WRITE_COMMANDS
#define LOAD_IDLE_MS 60000   /* abandon a paste that stops arriving */

/* CFG-6: read a JSON document from the console, validate it and (unless @p dry_run) save it
 * and restart. The document ends when its outer object closes; a line with a single '.'
 * aborts.
 * Issue #78: the paste can take up to LOAD_IDLE_MS. It is read WITHOUT s_ctx_lock (it only
 * touches this task's buffers), so gw_shell_set_ctx() from app_main never waits for an
 * operator; the lock is taken only to validate, save and restart, with the current context. */
typedef struct {
    char *buf;
    size_t len;
    bool aborted;
    bool overflow;
} pasted_t;

static bool config_read(pasted_t *p)
{
    /* grows with the document (up to the limit): without PSRAM 128 KiB at once may not exist */
    const size_t max = CONFIG_GW_CONFIG_MAX_BYTES + 1;
    size_t cap = 8192;
    char *buf = heap_caps_malloc_prefer(cap, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                        MALLOC_CAP_8BIT);
    if (buf == NULL) {
        out("config: no memory\n");
        return false;
    }
    gw_jsonfeed_t f;
    gw_jsonfeed_init(&f, buf, cap);
    out("paste the JSON document; it ends when its outer object closes ('.' alone aborts)\n");
    out_flush();
    char line[256];
    int64_t last = esp_timer_get_time();
    bool aborted = false;
    while (!f.complete && !aborted) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            clearerr(stdin);
            if (esp_timer_get_time() - last > LOAD_IDLE_MS * 1000LL) {
                aborted = true;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        last = esp_timer_get_time();
        aborted = strcmp(line, ".\n") == 0 || strcmp(line, ".\r\n") == 0 || strcmp(line, ".") == 0;
        size_t need = f.len + strlen(line) + 1;
        if (!aborted && need > f.cap && f.cap < max) {
            size_t ncap = 2 * f.cap > need ? 2 * f.cap : need;
            ncap = ncap < max ? ncap : max;
            char *nb = heap_caps_realloc_prefer(f.buf, ncap, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                                MALLOC_CAP_8BIT);
            if (nb != NULL) {
                f.buf = buf = nb;
                f.cap = ncap;
            }
        }
        if (!aborted) {
            gw_jsonfeed_add(&f, line);
        }
    }
    p->buf = buf;
    p->len = f.len;
    p->aborted = aborted;
    p->overflow = f.overflow;
    return true;
}

/* Runs with s_ctx_lock held: validate, save, restart. Frees the pasted buffer. */
static int config_apply(pasted_t *p, bool dry_run, bool discard_copy)
{
    int rc = 1;
    gw_cfg_err_t err;
    if (p->aborted) {
        out("config: aborted, nothing changed\n");
    } else if (p->overflow) {
        out("config: rejected: larger than %d bytes (CONFIG_GW_CONFIG_MAX_BYTES) or no memory\n",
               CONFIG_GW_CONFIG_MAX_BYTES);
    } else if (gw_storage_save_config_ex(p->buf, p->len, dry_run, discard_copy, &err) != ESP_OK) {
        out("config: rejected: %s\n", err.msg);
        if (err.path[0] != '\0') {
            out("config: path %s\n", err.path);
        } else if (err.line != 0) {
            out("config: line %u, column %u\n", err.line, err.column);
        }
    } else {
        out(dry_run ? "config: valid (%u bytes), nothing saved\n"
                       : "config: saved (%u bytes); restarting\n",
               (unsigned)p->len);
        rc = 0;
    }
    memset(p->buf, 0, p->len);   /* may hold secrets in clear */
    heap_caps_free(p->buf);
    p->buf = NULL;
    if (rc == 0 && !dry_run) {
        out_flush();
        s_ctx.restart();
    }
    return rc;
}

/* 'config load' / 'config check': registered without the ATOMIC wrapper (issue #78). */
static int cmd_config_load(bool dry_run, bool discard_copy)
{
    s_out_len = 0;
    pasted_t p;
    if (!config_read(&p)) {
        out_flush();
        return 1;
    }
    xSemaphoreTake(s_ctx_lock, portMAX_DELAY);
    int rc = config_apply(&p, dry_run, discard_copy);
    out_flush();
    xSemaphoreGive(s_ctx_lock);
    return rc;
}
#endif

static int cmd_config(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "show") == 0) {
        return config_show();
    }
    if (argc != 1) {
        out("usage: config [show%s]\n",
               CONFIG_GW_SHELL_WRITE_COMMANDS ? "|load|check" : "");
        return 1;
    }
    static const char *const parity[] = {"none", "even", "odd"};
    const gw_config_t *c = s_ctx.cfg;
    if (c == NULL && s_ctx.starting) {
        out("config: not available yet: the configuration is loading\n");
        return 1;
    }
    if (c == NULL) {
        out("REJECTED (%s): %s\n", s_ctx.config_source, s_ctx.config_error);
        return 1;
    }
    out("source     %s\n", s_ctx.config_source);
    out("sparkplug  group '%s', edge node '%s'\n", c->sparkplug.group_id,
           c->sparkplug.edge_node_id);
    out("mqtt       %s:%u, MQTT %s, client id '%s', keepalive %u s, user %s\n", c->mqtt.host,
           c->mqtt.port, c->mqtt.protocol == GW_MQTT_V5 ? "5" : "3.1.1", c->mqtt.client_id,
           c->mqtt.keepalive_s, c->mqtt.username[0] ? c->mqtt.username : "(none)");
    for (uint8_t i = 0; i < c->mqtt.backup_count; i++) {
        out("backup %u   %s:%u\n", i + 1u, c->mqtt.backup[i].host, c->mqtt.backup[i].port);
    }
    if (c->mqtt.backup_count > 0 && c->mqtt.primary_return_s > 0) {
        out("failover   after %u failed attempt(s); back to the primary after %lu s on a backup\n",
            c->mqtt.failover_after, (unsigned long)c->mqtt.primary_return_s);
    } else if (c->mqtt.backup_count > 0) {
        out("failover   after %u failed attempt(s); a working backup is kept\n",
            c->mqtt.failover_after);
    }
    const gw_tls_cfg_t *tls = &c->mqtt.tls;
    if (tls->enabled) {
        out("tls        CA %s%s, name checked against %s, client certificate %s%s\n",
            tls->ca_file[0] ? "/cfg/" : "ESP-IDF root bundle", tls->ca_file,
            tls->server_name[0] ? tls->server_name : "the broker host",
            tls->cert_file[0] ? "/cfg/" : "none", tls->cert_file);
    } else {
        out("tls        off (plain TCP)\n");
    }
    if (c->network.dhcp) {
        out("network    DHCP\n");
    } else {
        const uint8_t *a = c->network.ip, *m = c->network.netmask, *g = c->network.gateway;
        out("network    static %u.%u.%u.%u mask %u.%u.%u.%u gw %u.%u.%u.%u\n", a[0], a[1], a[2],
               a[3], m[0], m[1], m[2], m[3], g[0], g[1], g[2], g[3]);
    }
    const gw_wifi_cfg_t *w = &c->network.wifi;
    if (w->enabled) {
        out("wifi       '%s', %s, failback %u s, %s\n", w->ssid,
               w->role == GW_WIFI_PRIMARY ? "primary (Ethernet is the backup)" : "backup of Ethernet",
               w->failback_s, w->ipv4.dhcp ? "DHCP" : "static IPv4");
    } else {
        out("wifi       disabled\n");
    }
    const gw_ipv6_cfg_t *v6 = &c->network.ipv6;
    if (v6->enabled) {
        char a[48] = "none", d[48] = "none";
        ip6_addr_t t;
        if (v6->has_address) {
            memcpy(t.addr, v6->address, sizeof(t.addr));
            ip6addr_ntoa_r(&t, a, sizeof(a));
        }
        if (v6->has_dns) {
            memcpy(t.addr, v6->dns, sizeof(t.addr));
            ip6addr_ntoa_r(&t, d, sizeof(d));
        }
        for (char *ch = a; *ch != '\0'; ch++) {   /* RFC 5952: lower case */
            *ch = (char)tolower((unsigned char)*ch);
        }
        for (char *ch = d; *ch != '\0'; ch++) {
            *ch = (char)tolower((unsigned char)*ch);
        }
        out("ipv6       SLAAC %s, static address %s, DNS %s\n", v6->slaac ? "on" : "off", a, d);
    } else {
        out("ipv6       disabled\n");
    }
    out("ntp        %s\n", c->ntp.server);
    static const char *const levels[8] = {[3] = "error", [4] = "warning", [6] = "info",
                                          [7] = "debug"};
    if (c->syslog.host[0] != '\0') {
        out("syslog     %s:%u, level %s and above\n", c->syslog.host, c->syslog.port,
               levels[c->syslog.level & 7]);
    } else {
        out("syslog     disabled\n");
    }
    out("rs485      %lu baud, parity %s, %u stop bit(s), timeout %u ms, %u retries, "
           "inter-frame %u ms\n",
           (unsigned long)c->rs485.baud, parity[c->rs485.parity], c->rs485.stop_bits,
           c->rs485.response_timeout_ms, c->rs485.retries, c->rs485.inter_frame_delay_ms);
    out("local_io   DI debounce %u ms", c->local_io.di_debounce_ms);
    /* IO-3: one letter per DO, DO/1 first: o = off, 1 = on, h = hold */
    char modes[GW_LOCAL_IO_CHANNELS + 1];
    for (int i = 0; i < GW_LOCAL_IO_CHANNELS; i++) {
        modes[i] = "o1h"[c->local_io.do_failsafe[i] % 3];
    }
    modes[GW_LOCAL_IO_CHANNELS] = '\0';
    if (c->local_io.failsafe_s != 0) {
        out(", DO safe state %s (o off, 1 on, h hold) at restart and after %lu s without "
            "MQTT\n", modes, (unsigned long)c->local_io.failsafe_s);
    } else {
        out(", DO safe state %s (o off, 1 on, h hold) at restart; no MQTT failsafe\n", modes);
    }
    static const char *const rule_modes[] = {"follow", "invert", "toggle"};
    for (uint8_t i = 0; i < c->local_io.rule_count; i++) {   /* IO-7 */
        out("           rule %u: DO/%u %s DI/%u (local, NCMD refused)\n", i,
            c->local_io.rules[i].dout + 1u, rule_modes[c->local_io.rules[i].mode % 3],
            c->local_io.rules[i].di + 1u);
    }
    if (c->local_io.buzzer.alarms != 0) {
        uint8_t a = c->local_io.buzzer.alarms;
        out("           buzzer alarms:%s%s%s (after %u s)\n",
            a & GW_ALARM_BIT(GW_ALARM_CONFIG_ERROR) ? " config_error" : "",
            a & GW_ALARM_BIT(GW_ALARM_MQTT_DOWN) ? " mqtt_down" : "",
            a & GW_ALARM_BIT(GW_ALARM_DEVICE_OFFLINE) ? " device_offline" : "",
            c->local_io.buzzer.alarm_after_s);
    } else {
        out("           buzzer alarms: none\n");
    }
    if (c->local_io.counter_save_s != 0) {
        out("           DI pulse counters saved every %lu s\n",
            (unsigned long)c->local_io.counter_save_s);
    } else {
        out("           DI pulse counters not saved (lost at every restart)\n");
    }
    out("devices    %u device(s), %u tag(s)\n", c->device_count, c->tag_count);
    for (uint16_t d = 0; d < c->device_count; d++) {
        const gw_device_cfg_t *dev = &c->devices[d];
        unsigned slow = 0;
        for (uint16_t t = dev->first_tag; t < dev->first_tag + dev->tag_count; t++) {
            slow += c->tags[t].slow ? 1u : 0u;
        }
        /* #68: what the poll engine runs with. Block reads and gap: the device's keys, or the
         * build's defaults (CONFIG_GW_MB_BLOCK_READS / _MAX_GAP) when it has none. */
#if CONFIG_GW_MB_BLOCK_READS
        const bool build_merge = true;
        const int build_gap = CONFIG_GW_MB_BLOCK_MAX_GAP;
#else
        const bool build_merge = false;
        const int build_gap = 0;
#endif
        bool merge = dev->block_reads == GW_CFG_BUILD_DEFAULT ? build_merge : dev->block_reads != 0;
        int gap = dev->max_gap == GW_CFG_BUILD_DEFAULT ? build_gap : dev->max_gap;
        char blocks[48];
        if (merge) {
            snprintf(blocks, sizeof(blocks), "blocks max_gap %d%s", gap,
                     dev->max_gap == GW_CFG_BUILD_DEFAULT ? " (default)" : "");
        } else {
            snprintf(blocks, sizeof(blocks), "one request per tag%s",
                     dev->block_reads == GW_CFG_BUILD_DEFAULT ? " (default)" : "");
        }
        out("  %-16s unit %3u, poll %lu ms, slow %lu ms, offline after %u, %u tag(s) "
            "(%u fast, %u slow), %s\n",
            dev->name, dev->unit_id, (unsigned long)dev->poll_ms,
            (unsigned long)dev->slow_poll_ms, dev->offline_after, dev->tag_count,
            dev->tag_count - slow, slow, blocks);
    }
    if (c->modbus_tcp.enabled) {
        char clients[GW_MBTCP_MAX_ALLOWED * 20] = "";
        size_t used = 0;
        for (uint8_t i = 0; i < c->modbus_tcp.client_count && used < sizeof(clients); i++) {
            const uint8_t *ip = c->modbus_tcp.clients[i].ip;
            int n = snprintf(clients + used, sizeof(clients) - used, "%s%u.%u.%u.%u/%u",
                             i ? " " : "", ip[0], ip[1], ip[2], ip[3],
                             c->modbus_tcp.clients[i].prefix);
            used += n > 0 ? (size_t)n : 0;
        }
        out("modbus_tcp enabled, port %u, idle timeout %u s, clients %s\n", c->modbus_tcp.port,
            c->modbus_tcp.idle_timeout_s,
            c->modbus_tcp.client_count ? clients : "any (no authentication)");
    } else {
        out("modbus_tcp disabled\n");
    }
    return 0;
}

/* ICMP echo to an IPv4 or IPv6 literal: reachability over the active interface. */
static SemaphoreHandle_t s_ping_done;

/* The ping task prints from its own task: one locked write per line. */
static void line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void line(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        write_locked(buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
    }
}

static void on_ping_reply(esp_ping_handle_t h, void *arg)
{
    (void)arg;
    uint16_t seq;
    uint32_t ms;
    ip_addr_t from;
    char text[48];
    esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &ms, sizeof(ms));
    esp_ping_get_profile(h, ESP_PING_PROF_IPADDR, &from, sizeof(from));
    line("reply from %s: seq %u time %lu ms\n", ipaddr_ntoa_r(&from, text, sizeof(text)), seq,
           (unsigned long)ms);
}

static void on_ping_timeout(esp_ping_handle_t h, void *arg)
{
    (void)arg;
    uint16_t seq;
    esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    line("seq %u: timeout\n", seq);
}

static void on_ping_end(esp_ping_handle_t h, void *arg)
{
    (void)arg;
    uint32_t sent, received;
    esp_ping_get_profile(h, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
    esp_ping_get_profile(h, ESP_PING_PROF_REPLY, &received, sizeof(received));
    line("%lu sent, %lu received\n", (unsigned long)sent, (unsigned long)received);
    xSemaphoreGive(s_ping_done);
}

static int cmd_ping(int argc, char **argv)
{
    if (!up(GW_SHELL_UP_NET, "ping", "the network")) {
        return 1;
    }
    esp_ping_config_t pc = ESP_PING_DEFAULT_CONFIG();
    if (argc != 2 || !ipaddr_aton(argv[1], &pc.target_addr)) {
        out("usage: ping <IPv4 or IPv6 address>\n");
        return 1;
    }
    pc.count = 3;
    if (s_ping_done == NULL) {
        s_ping_done = xSemaphoreCreateBinary();
    }
    const esp_ping_callbacks_t cbs = {
        .on_ping_success = on_ping_reply, .on_ping_timeout = on_ping_timeout, .on_ping_end = on_ping_end};
    esp_ping_handle_t h;
    if (s_ping_done == NULL || esp_ping_new_session(&pc, &cbs, &h) != ESP_OK) {
        out("ping: cannot start\n");
        return 1;
    }
    esp_ping_start(h);
    xSemaphoreTake(s_ping_done, pdMS_TO_TICKS(10000));
    esp_ping_stop(h);
    esp_ping_delete_session(h);
    return 0;
}

#if CONFIG_GW_SHELL_WRITE_COMMANDS
static int cmd_restart(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    out("restarting\n");
    out_flush();
    s_ctx.restart();
    return 0;
}

static int cmd_rebirth(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (s_ctx.cfg != NULL && !up(GW_SHELL_UP_EDGE, "rebirth", "MQTT")) {
        return 1;
    }
    if (s_ctx.cfg == NULL || !gw_edge_request_rebirth()) {
        out("rebirth: no Sparkplug session\n");
        return 1;
    }
    out("rebirth: NBIRTH and every DBIRTH requested\n");
    return 0;
}

static int cmd_mbread(int argc, char **argv)
{
    static const char *const areas[] = {"coil", "discrete", "holding", "input"};
    static const mb_fc_t fcs[] = {MB_FC_READ_COILS, MB_FC_READ_DISCRETE_INPUTS,
                                  MB_FC_READ_HOLDING_REGISTERS, MB_FC_READ_INPUT_REGISTERS};
    int area = -1;
    for (int i = 0; argc >= 4 && i < 4; i++) {
        area = strcmp(argv[2], areas[i]) == 0 ? i : area;
    }
    long unit = argc >= 4 ? strtol(argv[1], NULL, 10) : 0;
    long addr = argc >= 4 ? strtol(argv[3], NULL, 10) : -1;
    long count = argc == 5 ? strtol(argv[4], NULL, 10) : 1;
    if (argc < 4 || argc > 5 || area < 0 || unit < 1 || unit > 247 || addr < 0 ||
        addr > 65535 || count < 1 || count > GW_BUS_DIAG_MAX || addr + count > 65536) {
        out("usage: mbread <unit 1-247> <coil|discrete|holding|input> <address> [count 1-%d]\n",
               GW_BUS_DIAG_MAX);
        return 1;
    }
    if (s_ctx.cfg == NULL && s_ctx.starting) {
        out("mbread: not available yet: the configuration is loading\n");
        return 1;
    }
    if (s_ctx.cfg == NULL) {
        out("mbread: the bus is not running (no valid configuration)\n");
        return 1;
    }
    if (!up(GW_SHELL_UP_BUS, "mbread", "the RS485 bus")) {
        return 1;
    }
    uint16_t v[GW_BUS_DIAG_MAX];
    uint8_t exc;
    /* worst case: a poll cycle in progress plus every retry of this request */
    uint32_t wait = 2000u + (uint32_t)s_ctx.cfg->rs485.response_timeout_ms *
                                (s_ctx.cfg->rs485.retries + 1u) * 2u;
    mb_status_t st = gw_bus_diag_read((uint8_t)unit, fcs[area], (uint16_t)addr, (uint16_t)count,
                                      v, &exc, wait);
    static const char *const status[] = {"ok", "timeout", "CRC error", "framing error",
                                         "exception", "busy or bus not answering in time"};
    if (st != MB_OK) {
        if (st == MB_ERR_EXCEPTION) {
            out("mbread: unit %ld %s %ld: exception %u\n", unit, areas[area], addr, exc);
        } else {
            out("mbread: unit %ld %s %ld: %s\n", unit, areas[area], addr,
                   status[st <= MB_ERR_ARG ? st : MB_ERR_ARG]);
        }
        return 1;
    }
    out("mbread: unit %ld %s %ld:", unit, areas[area], addr);
    for (long i = 0; i < count; i++) {
        out(" %u", v[i]);
    }
    out("\n");
    return 0;
}
#endif

#if CONFIG_GW_IO_SIMULATED
static int cmd_di(int argc, char **argv)
{
    long ch = argc == 3 ? strtol(argv[1], NULL, 10) : 0;
    long level = argc == 3 ? strtol(argv[2], NULL, 10) : -1;
    if (ch < 1 || ch > 8 || (level != 0 && level != 1)) {
        out("usage: di <1-8> <0|1>\n");
        return 1;
    }
    if (!up(GW_SHELL_UP_IO, "di", "the local I/O")) {
        return 1;
    }
    gw_io_sim_set_di((uint8_t)(ch - 1), level == 1);
    return 0;
}

static int cmd_boot(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "0") != 0 && strcmp(argv[1], "1") != 0)) {
        out("usage: boot <1 = press | 0 = release>\n");
        return 1;
    }
    gw_bootbtn_sim_set(argv[1][0] == '1');
    return 0;
}
/* Emulator only: crash on purpose, so the E2E can check the core dump in flash and the
 * "panic" reset reason. */
/* Issue #62: on the chip the panic handler stalls the other core (RTC_CNTL software stall)
 * while it writes the core dump; QEMU does not emulate that stall, so the other core kept
 * running tasks, changing their stacks and the task list between the two passes of the ELF
 * writer (truncated or overflowing dumps, nested aborts, wrong checksums). The emulator-only
 * 'panic' parks the other core first, as the hardware would, then aborts on a task pinned to
 * this side (a migrating console task could otherwise park itself). */
static void park_core(void *arg)
{
    (void)arg;
    portDISABLE_INTERRUPTS();
    for (;;) {
    }
}

static void panic_task(void *arg)
{
    (void)arg;
#if !CONFIG_FREERTOS_UNICORE
    esp_ipc_call(1, park_core, NULL);   /* returns once core 1 runs it: it never comes back */
#endif
    printf("panic requested from the console\n");
    fflush(stdout);
    abort();
}

static int cmd_panic(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (xTaskCreatePinnedToCore(panic_task, "panic", 3072, NULL, configMAX_PRIORITIES - 1, NULL,
                                0) != pdPASS) {
        printf("panic requested from the console (other core not parked)\n");
        fflush(stdout);
        abort();
    }
    vTaskDelay(portMAX_DELAY);
    return 0;
}
#endif

ATOMIC(cmd_status)
ATOMIC(cmd_devices)
ATOMIC(cmd_config)

/* Issue #78: 'config load|check' reads the paste without s_ctx_lock; the rest is atomic. */
static int cmd_config_entry(int argc, char **argv)
{
#if CONFIG_GW_SHELL_WRITE_COMMANDS
    if (argc == 2 && (strcmp(argv[1], "load") == 0 || strcmp(argv[1], "check") == 0)) {
        return cmd_config_load(strcmp(argv[1], "check") == 0, false);
    }
    /* issue #109: the explicit way out when LittleFS may hold an unreachable NVS migration copy */
    if (argc == 3 && strcmp(argv[1], "load") == 0 && strcmp(argv[2], "discard-nvs-copy") == 0) {
        return cmd_config_load(false, true);
    }
#endif
    return cmd_config_atomic(argc, argv);
}
ATOMIC(cmd_ping)
#if CONFIG_GW_SHELL_WRITE_COMMANDS
ATOMIC(cmd_restart)
ATOMIC(cmd_rebirth)
ATOMIC(cmd_mbread)
#endif
#if CONFIG_GW_IO_SIMULATED
ATOMIC(cmd_di)

/* Issue #74: the simulated expander stays silent for a while after the next boot. */
static int cmd_expander(int argc, char **argv)
{
    long ms = argc == 3 && strcmp(argv[1], "absent") == 0 ? strtol(argv[2], NULL, 10) : -1;
    if (ms < 1 || ms > 600000) {
        out("usage: expander absent <1-600000 ms>\n");
        return 1;
    }
    gw_io_sim_expander_absent((uint32_t)ms);
    out("expander silent for %ld ms after the next boot\n", ms);
    return 0;
}

/* Issue #84: the next configuration validation keeps its buffers for a while at its peak. */
static int cmd_validation(int argc, char **argv)
{
    long ms = argc == 3 && strcmp(argv[1], "hold") == 0 ? strtol(argv[2], NULL, 10) : -1;
    if (ms < 1 || ms > 60000) {
        out("usage: validation hold <1-60000 ms>\n");
        return 1;
    }
    gw_storage_sim_hold_validation((uint32_t)ms);
    out("the next configuration validation is held for %ld ms\n", ms);
    return 0;
}

ATOMIC(cmd_expander)
ATOMIC(cmd_validation)
ATOMIC(cmd_boot)
#endif

static esp_err_t new_repl(const esp_console_repl_config_t *rc, esp_console_repl_t **repl)
{
#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    const esp_console_dev_uart_config_t dev = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    return esp_console_new_repl_uart(&dev, rc, repl);
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    const esp_console_dev_usb_serial_jtag_config_t dev =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    return esp_console_new_repl_usb_serial_jtag(&dev, rc, repl);
#else
    (void)rc;
    (void)repl;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t gw_shell_start(const gw_shell_ctx_t *ctx)
{
    s_ctx = *ctx;
    s_ctx_lock = xSemaphoreCreateMutexStatic(&s_ctx_lock_buf);
    s_console = xSemaphoreCreateRecursiveMutexStatic(&s_console_buf);
    s_prev_vprintf = esp_log_set_vprintf(log_vprintf);
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "gw>";
    esp_console_repl_t *repl;
    ESP_RETURN_ON_ERROR(new_repl(&rc, &repl), TAG, "console REPL");

    static const esp_console_cmd_t cmds[] = {
        {.command = "status", .help = "Gateway, configuration, network, MQTT and I/O state",
         .func = cmd_status_atomic},
        {.command = "devices", .help = "Modbus devices: online state and counters",
         .func = cmd_devices_atomic},
        {.command = "config",
         .help = "Summary of the loaded configuration; 'show' prints the stored document "
                 "(secrets masked)"
#if CONFIG_GW_SHELL_WRITE_COMMANDS
                 ", 'load' reads a new one from the console, validates, saves and restarts, "
                 "'check' only validates; 'load discard-nvs-copy' also formats a LittleFS that "
                 "does not mount, losing an NVS migration copy it may hold (README 6.2.1)"
#endif
         ,
         .hint = "[show|load [discard-nvs-copy]|check]", .func = cmd_config_entry},
#if CONFIG_GW_SHELL_WRITE_COMMANDS
        {.command = "restart", .help = "Orderly restart (NDEATH first)", .func = cmd_restart_atomic},
        {.command = "rebirth", .help = "Re-issue NBIRTH and every DBIRTH", .func = cmd_rebirth_atomic},
        {.command = "mbread", .help = "One Modbus read through the bus owner",
         .hint = "<unit> <coil|discrete|holding|input> <address> [count]", .func = cmd_mbread_atomic},
#endif
        {.command = "ping", .help = "ICMP echo (3 requests) to an IPv4 or IPv6 address",
         .hint = "<address>", .func = cmd_ping_atomic},
#if CONFIG_GW_IO_SIMULATED
        {.command = "di", .help = "Emulator only: set a simulated input", .hint = "<1-8> <0|1>",
         .func = cmd_di_atomic},
        {.command = "expander", .help = "Emulator only: the DO expander does not answer for a "
         "while after the next boot (issue #74)", .hint = "absent <ms>", .func = cmd_expander_atomic},
        {.command = "validation", .help = "Emulator only: the next configuration validation "
         "keeps its buffers for a while (issue #84)", .hint = "hold <ms>", .func = cmd_validation_atomic},
        {.command = "boot", .help = "Emulator only: press (1) or release (0) the BOOT button",
         .hint = "<0|1>", .func = cmd_boot_atomic},
        {.command = "panic", .help = "Emulator only: abort() (core dump test)", .func = cmd_panic},
#endif
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmds[i]), TAG, "%s", cmds[i].command);
    }
    ESP_RETURN_ON_ERROR(esp_console_register_help_command(), TAG, "help");
    ESP_RETURN_ON_ERROR(esp_console_start_repl(repl), TAG, "REPL");
    ESP_LOGI(TAG, "console ready");
    return ESP_OK;
}
