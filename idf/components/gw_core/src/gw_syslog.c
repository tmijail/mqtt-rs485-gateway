#include "gw_syslog.h"

#include <stdio.h>
#include <string.h>

int gw_syslog_format_severity(const char *fmt)
{
    if (fmt[0] == '\x1b') {   /* colour prefix "\x1b[0;31m" */
        const char *m = strchr(fmt, 'm');
        fmt = m != NULL ? m + 1 : fmt;
    }
    switch (fmt[0]) {
    case 'E': return 3;
    case 'W': return 4;
    case 'I': return 6;
    case 'D':
    case 'V': return 7;
    default: return -1;
    }
}

/* Skip one ANSI "ESC [ ... m" sequence at @p p, if there is one. */
static const char *skip_colour(const char *p)
{
    if (p[0] == '\x1b' && p[1] == '[') {
        const char *m = strchr(p, 'm');
        return m != NULL ? m + 1 : p;
    }
    return p;
}

static void copy_trimmed(char *out, size_t cap, const char *p, size_t n)
{
    /* drop the trailing newline and colour reset of esp_log */
    while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == '\r')) {
        n--;
    }
    if (n >= 4 && memcmp(p + n - 4, "\x1b[0m", 4) == 0) {
        n -= 4;
    }
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, p, n);
    out[n] = '\0';
}

bool gw_syslog_parse_line(const char *line, gw_syslog_rec_t *rec)
{
    const char *p = skip_colour(line);
    int sev = gw_syslog_format_severity(p);
    /* "<L> (<timestamp>) <tag>: <message>" */
    const char *close = (sev >= 0 && p[1] == ' ' && p[2] == '(') ? strchr(p + 3, ')') : NULL;
    const char *tag = close != NULL && close[1] == ' ' ? close + 2 : NULL;
    const char *colon = tag != NULL ? strstr(tag, ": ") : NULL;
    if (colon == NULL) {
        rec->severity = 6;
        strcpy(rec->tag, "-");
        copy_trimmed(rec->msg, sizeof(rec->msg), line, strlen(line));
        return false;
    }
    rec->severity = (uint8_t)sev;
    copy_trimmed(rec->tag, sizeof(rec->tag), tag, (size_t)(colon - tag));
    copy_trimmed(rec->msg, sizeof(rec->msg), colon + 2, strlen(colon + 2));
    return true;
}

/* Civil date from days since 1970-01-01 (H. Hinnant's algorithm). */
static void civil(int64_t days, int *y, unsigned *m, unsigned *d)
{
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yoe + (unsigned)era * 400u) + (*m <= 2);
}

void gw_syslog_timestamp(uint64_t time_ms, char out[25])
{
    uint64_t s = time_ms / 1000;
    int y;
    unsigned mo, d;
    civil((int64_t)(s / 86400), &y, &mo, &d);
    unsigned sod = (unsigned)(s % 86400);
    char tmp[48];   /* the compiler cannot bound the fields; they always fit in 24 */
    snprintf(tmp, sizeof(tmp), "%04d-%02u-%02uT%02u:%02u:%02u.%03uZ", y % 10000, mo, d, sod / 3600,
             sod / 60 % 60, sod % 60, (unsigned)(time_ms % 1000));
    memcpy(out, tmp, 24);
    out[24] = '\0';
}

/* PRINTUSASCII without spaces; "-" when empty. */
static size_t put_token(char *out, size_t cap, const char *s, size_t max)
{
    size_t n = 0;
    for (; s[n] != '\0' && n < max && n + 1 < cap; n++) {
        unsigned char c = (unsigned char)s[n];
        out[n] = (c > 32 && c < 127) ? (char)c : '_';
    }
    if (n == 0 && cap > 1) {
        out[n++] = '-';
    }
    out[n] = '\0';
    return n;
}

size_t gw_syslog_format(const gw_syslog_rec_t *rec, const char *hostname, uint32_t sequence,
                        char *out, size_t cap)
{
    if (cap == 0) {
        return 0;
    }
    char ts[25] = "-";
    if (rec->time_ms != 0) {
        gw_syslog_timestamp(rec->time_ms, ts);
    }
    char host[65], app[GW_SYSLOG_TAG_MAX];
    put_token(host, sizeof(host), hostname, 64);
    put_token(app, sizeof(app), rec->tag, 48);
    int n = snprintf(out, cap, "<%u>1 %s %s %s - - [meta sequenceId=\"%lu\"] ",
                     (unsigned)(GW_SYSLOG_FACILITY * 8 + (rec->severity & 7)), ts, host, app,
                     (unsigned long)sequence);
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    size_t len = (size_t)n < cap ? (size_t)n : cap - 1;
    for (const char *m = rec->msg; *m != '\0' && len + 1 < cap; m++) {
        unsigned char c = (unsigned char)*m;
        out[len++] = (c < 32 || c == 127) ? ' ' : (char)c;
    }
    out[len] = '\0';
    return len;
}

/* ---- ring ---- */

void gw_syslog_ring_init(gw_syslog_ring_t *r, gw_syslog_rec_t *slots, unsigned count)
{
    r->slots = slots;
    r->count = count;
    atomic_store(&r->head, 0);
    atomic_store(&r->tail, 0);
    atomic_store(&r->dropped, 0);
}

static unsigned next(const gw_syslog_ring_t *r, unsigned i)
{
    return i + 1 == r->count ? 0 : i + 1;
}

gw_syslog_rec_t *gw_syslog_ring_reserve(gw_syslog_ring_t *r)
{
    unsigned head = atomic_load_explicit(&r->head, memory_order_relaxed);
    if (r->count < 2 || next(r, head) == atomic_load_explicit(&r->tail, memory_order_acquire)) {
        atomic_fetch_add(&r->dropped, 1);
        return NULL;
    }
    return &r->slots[head];
}

void gw_syslog_ring_commit(gw_syslog_ring_t *r)
{
    unsigned head = atomic_load_explicit(&r->head, memory_order_relaxed);
    atomic_store_explicit(&r->head, next(r, head), memory_order_release);
}

const gw_syslog_rec_t *gw_syslog_ring_peek(gw_syslog_ring_t *r)
{
    unsigned tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    if (tail == atomic_load_explicit(&r->head, memory_order_acquire)) {
        return NULL;
    }
    return &r->slots[tail];
}

void gw_syslog_ring_pop(gw_syslog_ring_t *r)
{
    unsigned tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    atomic_store_explicit(&r->tail, next(r, tail), memory_order_release);
}

unsigned gw_syslog_ring_take_dropped(gw_syslog_ring_t *r)
{
    return atomic_exchange(&r->dropped, 0);
}

void gw_syslog_ring_count_drop(gw_syslog_ring_t *r)
{
    atomic_fetch_add(&r->dropped, 1);
}
