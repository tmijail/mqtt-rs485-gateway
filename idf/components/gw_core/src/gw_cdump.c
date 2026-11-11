#include "gw_cdump.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *gw_cdump_state_name(gw_cdump_state_t s)
{
    switch (s) {
    case GW_CDUMP_VALID:
        return "valid";
    case GW_CDUMP_CORRUPT:
        return "corrupt";
    default:
        return "none";
    }
}

/* ---------- a bounded text builder ---------- */

typedef struct {
    char *out;
    size_t cap;
    size_t len;
    bool over;   /* something did not fit */
} buf_t;

static void put(buf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void put(buf_t *b, const char *fmt, ...)
{
    if (b->over) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->out + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->cap - b->len) {
        b->over = true;
        return;
    }
    b->len += (size_t)n;
}

/* A quoted JSON string: '"' and '\' escaped, control characters as \u00XX. */
static void put_str(buf_t *b, const char *s)
{
    put(b, "\"");
    for (const unsigned char *c = (const unsigned char *)s; *c != '\0' && !b->over; c++) {
        if (*c == '"' || *c == '\\') {
            put(b, "\\%c", *c);
        } else if (*c < 0x20) {
            put(b, "\\u%04x", *c);
        } else {
            put(b, "%c", *c);
        }
    }
    put(b, "\"");
}

static size_t finish(buf_t *b)
{
    if (!b->over) {
        return b->len;
    }
    if (b->cap >= 3) {
        memcpy(b->out, "{}", 3);
    } else if (b->cap > 0) {
        b->out[0] = '\0';
    }
    return 0;
}

static void status_fields(buf_t *b, gw_cdump_state_t state, uint32_t size)
{
    put(b, "\"present\":%s,\"state\":\"%s\",\"size\":%lu", state != GW_CDUMP_NONE ? "true" : "false",
        gw_cdump_state_name(state), (unsigned long)size);
}

size_t gw_cdump_status_json(gw_cdump_state_t state, uint32_t size, char *out, size_t cap)
{
    buf_t b = {.out = out, .cap = cap};
    if (cap == 0) {
        return 0;
    }
    put(&b, "{");
    status_fields(&b, state, size);
    put(&b, "}");
    return finish(&b);
}

size_t gw_cdump_summary_json(const gw_cdump_info_t *i, const char *reset_reason, char *out,
                             size_t cap)
{
    buf_t b = {.out = out, .cap = cap};
    if (cap == 0) {
        return 0;
    }
    put(&b, "{");
    status_fields(&b, i->state, i->size);
    put(&b, ",\"reset_reason\":");
    put_str(&b, reset_reason != NULL ? reset_reason : "");
    if (i->state == GW_CDUMP_VALID && i->has_summary) {
        put(&b, ",\"task\":");
        put_str(&b, i->task);
        put(&b, ",\"pc\":\"0x%08lx\",\"backtrace\":[", (unsigned long)i->pc);
        uint8_t depth = i->depth <= GW_CDUMP_BT_MAX ? i->depth : GW_CDUMP_BT_MAX;
        for (uint8_t k = 0; k < depth; k++) {
            put(&b, "%s\"0x%08lx\"", k ? "," : "", (unsigned long)i->bt[k]);
        }
        put(&b, "],\"backtrace_corrupted\":%s,\"exc_cause\":%lu,\"exc_vaddr\":\"0x%08lx\"",
            i->bt_corrupted ? "true" : "false", (unsigned long)i->exc_cause,
            (unsigned long)i->exc_vaddr);
        put(&b, ",\"reason\":");
        put_str(&b, i->reason);
        put(&b, ",\"app_elf_sha256\":");
        put_str(&b, i->app_sha256);
    }
    put(&b, "}\n");
    return finish(&b);
}

size_t gw_cdump_text(const gw_cdump_info_t *i, const char *reset_reason, char *out, size_t cap)
{
    buf_t b = {.out = out, .cap = cap};
    if (cap == 0) {
        return 0;
    }
    if (i->state == GW_CDUMP_NONE) {
        put(&b, "coredump  none in flash\n");
    } else {
        put(&b, "coredump  %lu bytes in flash, %s; the reset reason of this boot: %s\n",
            (unsigned long)i->size,
            i->state == GW_CDUMP_VALID ? "checksum correct" : "CORRUPT (wrong checksum or size)",
            reset_reason != NULL ? reset_reason : "?");
    }
    if (i->state == GW_CDUMP_VALID && i->has_summary) {
        put(&b, "task      %s\npc        0x%08lx\nbacktrace", i->task, (unsigned long)i->pc);
        uint8_t depth = i->depth <= GW_CDUMP_BT_MAX ? i->depth : GW_CDUMP_BT_MAX;
        for (uint8_t k = 0; k < depth; k++) {
            put(&b, " 0x%08lx", (unsigned long)i->bt[k]);
        }
        put(&b, "%s\nexception cause %lu, address 0x%08lx\n",
            i->bt_corrupted ? " |<-CORRUPTED" : "", (unsigned long)i->exc_cause,
            (unsigned long)i->exc_vaddr);
        if (i->reason[0] != '\0') {   /* one line (a watchdog's reason has several) */
            put(&b, "reason    ");
            for (const unsigned char *c = (const unsigned char *)i->reason; *c != '\0'; c++) {
                put(&b, "%c", *c < 0x20 ? ' ' : *c);
            }
            put(&b, "\n");
        }
        if (i->app_sha256[0] != '\0') {
            put(&b, "app ELF   sha256 %s...\n", i->app_sha256);
        }
    }
    if (b.over) {   /* truncated: keep what fits, ended by a newline */
        b.len = cap - 1;
        if (b.len > 0) {
            out[b.len - 1] = '\n';
        }
        out[b.len] = '\0';
    }
    return b.len;
}

size_t gw_cdump_b64(const uint8_t *in, size_t len, char *out, size_t cap)
{
    static const char k[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = (len + 2) / 3 * 4;
    if (cap < need + 1) {
        if (cap > 0) {
            out[0] = '\0';
        }
        return 0;
    }
    size_t n = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) {
            v |= (uint32_t)in[i + 1] << 8;
        }
        if (i + 2 < len) {
            v |= in[i + 2];
        }
        out[n++] = k[(v >> 18) & 63u];
        out[n++] = k[(v >> 12) & 63u];
        out[n++] = i + 1 < len ? k[(v >> 6) & 63u] : '=';
        out[n++] = i + 2 < len ? k[v & 63u] : '=';
    }
    out[n] = '\0';
    return n;
}
