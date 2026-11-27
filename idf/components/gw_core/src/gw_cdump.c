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

/* ---------- 'coredump dump' (#132, #293) ---------- */

_Static_assert(GW_CDUMP_WRITE_LINES * (GW_CDUMP_LINE_CHARS + 1) + 1 <= GW_CDUMP_TEXT_SIZE,
               "the lines of one write fit into the text buffer");
_Static_assert(GW_CDUMP_LINE_BYTES % 3 == 0 && GW_CDUMP_LINE_CHARS == GW_CDUMP_LINE_BYTES / 3 * 4,
               "a complete line has no '=' padding, so the lines join into one base64 text");

uint32_t gw_cdump_line_count(uint32_t size)
{
    return size / GW_CDUMP_LINE_BYTES + (size % GW_CDUMP_LINE_BYTES != 0 ? 1u : 0u);
}

/* One line through io->write; false when the console did not take it. */
static bool say(const gw_cdump_io_t *io, gw_cdump_work_t *w, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static bool say(const gw_cdump_io_t *io, gw_cdump_work_t *w, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->text, sizeof(w->text), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return false;
    }
    size_t len = (size_t)n < sizeof(w->text) ? (size_t)n : sizeof(w->text) - 1;
    return io->write(io->ctx, w->text, len);
}

/* One pass over the dump: the SHA-256 of its bytes into w->sha. With @p print, also the base64
 * lines through io->write, GW_CDUMP_WRITE_LINES lines in each write. Returns GW_CDUMP_COPY_OK,
 * _READ (with *err set) or _STALLED. */
static gw_cdump_copy_t pass(const gw_cdump_io_t *io, uint32_t size, gw_cdump_work_t *w,
                            bool print, const char **err)
{
    const uint32_t chunk = (uint32_t)sizeof(w->bin);
    size_t tlen = 0;
    unsigned lines = 0;
    *err = NULL;
    gw_sha256_init(&w->sha);
    for (uint32_t off = 0; off < size; off += chunk) {
        size_t n = size - off < chunk ? size - off : chunk;
        *err = io->read(io->ctx, off, w->bin, n);
        if (*err != NULL) {
            break;
        }
        gw_sha256_update(&w->sha, w->bin, n);
        for (size_t k = 0; print && k < n; k += GW_CDUMP_LINE_BYTES) {
            size_t m = n - k < GW_CDUMP_LINE_BYTES ? n - k : GW_CDUMP_LINE_BYTES;
            tlen += gw_cdump_b64(w->bin + k, m, w->text + tlen, sizeof(w->text) - tlen);
            w->text[tlen++] = '\n';
            if (++lines == GW_CDUMP_WRITE_LINES) {
                if (!io->write(io->ctx, w->text, tlen)) {
                    return GW_CDUMP_COPY_STALLED;
                }
                tlen = 0;
                lines = 0;
            }
        }
    }
    /* the lines before the end, or before a read error */
    if (tlen > 0 && !io->write(io->ctx, w->text, tlen)) {
        return GW_CDUMP_COPY_STALLED;
    }
    return *err != NULL ? GW_CDUMP_COPY_READ : GW_CDUMP_COPY_OK;
}

gw_cdump_copy_t gw_cdump_copy(const gw_cdump_io_t *io, gw_cdump_state_t state, uint32_t size,
                              gw_cdump_work_t *w)
{
    static const char hexd[] = "0123456789abcdef";
    uint8_t first[GW_SHA256_LEN];
    uint8_t second[GW_SHA256_LEN];
    char hex[2 * GW_SHA256_LEN + 1];
    const char *err = NULL;
    const uint32_t lines = gw_cdump_line_count(size);

    /* the first read: the SHA-256 for the first line */
    gw_cdump_copy_t rc = pass(io, size, w, false, &err);
    if (rc == GW_CDUMP_COPY_OK) {
        gw_sha256_final(&w->sha, first);
        for (size_t i = 0; i < GW_SHA256_LEN; i++) {
            hex[2 * i] = hexd[first[i] >> 4];
            hex[2 * i + 1] = hexd[first[i] & 15u];
        }
        hex[2 * GW_SHA256_LEN] = '\0';
        if (!say(io, w, "coredump: %lu bytes (%s), %lu lines of base64, sha256 %s\n",
                 (unsigned long)size, gw_cdump_state_name(state), (unsigned long)lines, hex) ||
            !say(io, w, "coredump: the dump can contain secrets (passwords, keys): keep the "
                        "copy safe\n") ||
            !say(io, w, "-----BEGIN CORE DUMP-----\n")) {
            rc = GW_CDUMP_COPY_STALLED;
        }
    }
    /* the second read: the lines, and the SHA-256 again */
    if (rc == GW_CDUMP_COPY_OK) {
        rc = pass(io, size, w, true, &err);
    }
    if (rc == GW_CDUMP_COPY_OK) {
        gw_sha256_final(&w->sha, second);
        if (memcmp(first, second, sizeof(first)) != 0) {
            rc = GW_CDUMP_COPY_CHANGED;
        } else if (!say(io, w, "-----END CORE DUMP----- %lu lines\n", (unsigned long)lines)) {
            rc = GW_CDUMP_COPY_STALLED;
        }
    }
    switch (rc) {
    case GW_CDUMP_COPY_READ:
        (void)say(io, w, "-----CORE DUMP INCOMPLETE: %s-----\n", err != NULL ? err : "?");
        break;
    case GW_CDUMP_COPY_CHANGED:
        (void)say(io, w, "-----CORE DUMP INCOMPLETE: the dump changed during the copy-----\n");
        break;
    case GW_CDUMP_COPY_STALLED:
        /* the console can take it later; if it does not, the END marker is absent also */
        (void)say(io, w, "-----CORE DUMP INCOMPLETE: the console did not read the output in "
                         "time-----\n");
        break;
    default:
        break;
    }
    memset(w, 0, sizeof(*w));   /* the dump can hold secrets */
    return rc;
}
