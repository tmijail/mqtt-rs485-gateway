#include "gw_cardfmt.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *const k_card_state[GW_CARD_STATE_COUNT] = {
    "unsupported", "unchecked", "absent", "not_mounted", "mounted", "formatting", "formatted",
};

const char *gw_card_state_name(gw_card_state_t s)
{
    return (unsigned)s < GW_CARD_STATE_COUNT ? k_card_state[s] : "?";
}

/* A bounded writer: once something does not fit, everything after it is dropped and the
 * writer remembers it (the JSON callers then answer "{}" rather than a cut object). */
typedef struct {
    char *out;
    size_t cap;
    size_t len;
    bool cut;
} wr_t;

static void wr(wr_t *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void wr(wr_t *w, const char *fmt, ...)
{
    if (w->cut || w->cap == 0) {
        w->cut = true;
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->out + w->len, w->cap - w->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= w->cap - w->len) {
        w->cut = true;
        w->len = w->cap - 1;   /* vsnprintf wrote what fits, NUL terminated */
        return;
    }
    w->len += (size_t)n;
}

/* A JSON string (quotes included); control characters as \u00XX. */
static void wr_str(wr_t *w, const char *s)
{
    wr(w, "\"");
    for (const unsigned char *c = (const unsigned char *)s; *c != '\0' && !w->cut; c++) {
        if (*c == '"' || *c == '\\') {
            wr(w, "\\%c", *c);
        } else if (*c < 0x20) {
            wr(w, "\\u%04x", *c);
        } else {
            wr(w, "%c", *c);
        }
    }
    wr(w, "\"");
}

static size_t json_end(wr_t *w)
{
    if (w->cut) {
        if (w->cap >= 3) {
            memcpy(w->out, "{}", 3);
        } else if (w->cap > 0) {
            w->out[0] = '\0';
        }
        return 0;
    }
    return w->len;
}

size_t gw_card_text(const gw_card_info_t *c, char *out, size_t cap)
{
    wr_t w = {out, cap, 0, false};
    if (cap > 0) {
        out[0] = '\0';
    }
    switch (c->state) {
    case GW_CARD_MOUNTED:
        wr(&w, "mounted:");
        break;
    case GW_CARD_UNMOUNTED:
        wr(&w, "NOT MOUNTED:");
        break;
    case GW_CARD_ABSENT:
        wr(&w, "absent:");
        break;
    case GW_CARD_FORMATTING:
        wr(&w, "formatting:");
        break;
    case GW_CARD_FORMATTED:
        wr(&w, "formatted:");
        break;
    default:
        wr(&w, "%s:", gw_card_state_name(c->state));
        break;
    }
    const char *sep = " ";
    if (c->name[0] != '\0') {
        wr(&w, "%smicroSD %s", sep, c->name);
        sep = ", ";
    }
    if (c->size_mb != 0) {
        wr(&w, "%s%lu MB", sep, (unsigned long)c->size_mb);
        sep = ", ";
    }
    if (c->fat[0] != '\0') {
        wr(&w, "%s%s", sep, c->fat);
        sep = ", ";
    }
    if (c->cluster != 0) {
        wr(&w, "%s%lu B clusters", sep, (unsigned long)c->cluster);
        sep = ", ";
    }
    if (c->free_mb != GW_CARD_UNKNOWN) {
        wr(&w, "%s%lu MB free", sep, (unsigned long)c->free_mb);
        sep = ", ";
    }
    if (c->other != GW_CARD_UNKNOWN) {
        wr(&w, "%s%lu other entr%s in the root", sep, (unsigned long)c->other,
           c->other == 1 ? "y" : "ies");
        sep = ", ";
    }
    if (c->detail[0] != '\0') {
        wr(&w, "%s%s", sep, c->detail);
    }
    return w.len;
}

size_t gw_card_json(const gw_card_info_t *c, char *out, size_t cap)
{
    wr_t w = {out, cap, 0, false};
    wr(&w, "{\"state\":\"%s\"", gw_card_state_name(c->state));
    if (c->detail[0] != '\0') {
        wr(&w, ",\"detail\":");
        wr_str(&w, c->detail);
    }
    if (c->name[0] != '\0') {
        wr(&w, ",\"name\":");
        wr_str(&w, c->name);
    }
    if (c->size_mb != 0) {
        wr(&w, ",\"size_mb\":%lu", (unsigned long)c->size_mb);
    }
    if (c->fat[0] != '\0') {
        wr(&w, ",\"fat\":");
        wr_str(&w, c->fat);
    }
    if (c->cluster != 0) {
        wr(&w, ",\"cluster\":%lu", (unsigned long)c->cluster);
    }
    if (c->free_mb != GW_CARD_UNKNOWN) {
        wr(&w, ",\"free_mb\":%lu", (unsigned long)c->free_mb);
    }
    if (c->other != GW_CARD_UNKNOWN) {
        wr(&w, ",\"other_entries\":%lu", (unsigned long)c->other);
    }
    wr(&w, "}");
    return json_end(&w);
}

/* ---- the format ---- */

static const char *const k_phase[] = {"idle", "running", "done", "failed"};

const char *gw_cardfmt_phase_name(gw_cardfmt_phase_t p)
{
    return (unsigned)p < sizeof(k_phase) / sizeof(k_phase[0]) ? k_phase[p] : "?";
}

static const struct {
    const char *key, *http, *text;
} k_answer[] = {
    [GW_CARDFMT_STARTED] = {"started", "202 Accepted",
                            "started in the background (a large card can take some minutes); "
                            "see 'card' or 'status'"},
    [GW_CARDFMT_BUSY] = {"busy", "409 Conflict", "a format runs already"},
    /* "this build has no microSD card" is what the E2E expects of the QEMU build */
    [GW_CARDFMT_NO_SUPPORT] = {"no_support", "501 Not Implemented",
                               "this build has no microSD card (QEMU: a LittleFS directory "
                               "stands in for it, or CONFIG_GW_SF_SPILL_NONE); nothing "
                               "formatted"},
    [GW_CARDFMT_NO_PSRAM] = {"no_psram", "501 Not Implemented",
                             "the format needs PSRAM, and none was detected; nothing formatted"},
    [GW_CARDFMT_NO_MEMORY] = {"no_memory", "503 Service Unavailable",
                              "no memory for the format task; nothing formatted"},
};

#define ANSWERS (sizeof(k_answer) / sizeof(k_answer[0]))

const char *gw_cardfmt_answer_key(gw_cardfmt_answer_t a)
{
    return (unsigned)a < ANSWERS ? k_answer[a].key : "?";
}

const char *gw_cardfmt_answer_http(gw_cardfmt_answer_t a)
{
    return (unsigned)a < ANSWERS ? k_answer[a].http : "500 Internal Server Error";
}

const char *gw_cardfmt_answer_text(gw_cardfmt_answer_t a)
{
    return (unsigned)a < ANSWERS ? k_answer[a].text : "?";
}

gw_cardfmt_answer_t gw_cardfmt_request(gw_cardfmt_t *st, bool support, bool psram,
                                       bool simulate, uint32_t now_ms)
{
    if (st->phase == GW_CARDFMT_RUNNING) {
        return GW_CARDFMT_BUSY;
    }
    if (!simulate && !support) {
        return GW_CARDFMT_NO_SUPPORT;
    }
    if (!simulate && !psram) {
        return GW_CARDFMT_NO_PSRAM;
    }
    uint32_t count = st->count;
    memset(st, 0, sizeof(*st));
    st->phase = GW_CARDFMT_RUNNING;
    st->simulated = simulate;
    st->start_ms = now_ms;
    st->count = count + 1;
    return GW_CARDFMT_STARTED;
}

static void copy(char *dst, size_t cap, const char *src)
{
    size_t n = src != NULL ? strnlen(src, cap - 1) : 0;
    if (n > 0) {
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

void gw_cardfmt_done(gw_cardfmt_t *st, uint32_t now_ms, const char *fat, uint32_t cluster,
                     uint32_t size_mb)
{
    st->phase = GW_CARDFMT_DONE;
    st->took_ms = now_ms - st->start_ms;
    copy(st->fat, sizeof(st->fat), fat);
    st->cluster = cluster;
    st->size_mb = size_mb;
    st->why[0] = '\0';
}

void gw_cardfmt_failed(gw_cardfmt_t *st, uint32_t now_ms, const char *why)
{
    st->phase = GW_CARDFMT_FAILED;
    st->took_ms = now_ms - st->start_ms;
    copy(st->why, sizeof(st->why), why);
}

uint32_t gw_cardfmt_elapsed_ms(const gw_cardfmt_t *st, uint32_t now_ms)
{
    switch (st->phase) {
    case GW_CARDFMT_RUNNING:
        return now_ms - st->start_ms;   /* unsigned: right across a wrap of the clock */
    case GW_CARDFMT_DONE:
    case GW_CARDFMT_FAILED:
        return st->took_ms;
    default:
        return 0;
    }
}

size_t gw_cardfmt_text(const gw_cardfmt_t *st, uint32_t now_ms, char *out, size_t cap)
{
    wr_t w = {out, cap, 0, false};
    if (cap > 0) {
        out[0] = '\0';
    }
    uint32_t s = gw_cardfmt_elapsed_ms(st, now_ms) / 1000u;
    const char *sim = st->simulated ? " (simulated: nothing written)" : "";
    switch (st->phase) {
    case GW_CARDFMT_IDLE:
        wr(&w, "idle: no format since the boot");
        break;
    case GW_CARDFMT_RUNNING:
        wr(&w, "running for %lu s%s", (unsigned long)s, sim);
        break;
    case GW_CARDFMT_DONE:
        wr(&w, "done in %lu s%s: %s, %lu B clusters, %lu MB; restart the gateway to use the card",
           (unsigned long)s, sim, st->fat, (unsigned long)st->cluster,
           (unsigned long)st->size_mb);
        break;
    case GW_CARDFMT_FAILED:
        wr(&w, "FAILED after %lu s%s: %s", (unsigned long)s, sim, st->why);
        break;
    }
    return w.len;
}

size_t gw_cardfmt_json(const gw_cardfmt_t *st, uint32_t now_ms, char *out, size_t cap)
{
    wr_t w = {out, cap, 0, false};
    uint32_t s = gw_cardfmt_elapsed_ms(st, now_ms) / 1000u;
    wr(&w, "{\"state\":\"%s\"", gw_cardfmt_phase_name(st->phase));
    if (st->simulated) {
        wr(&w, ",\"simulated\":true");
    }
    if (st->phase == GW_CARDFMT_RUNNING) {
        wr(&w, ",\"elapsed_s\":%lu", (unsigned long)s);
    } else if (st->phase == GW_CARDFMT_DONE) {
        wr(&w, ",\"duration_s\":%lu,\"fat\":", (unsigned long)s);
        wr_str(&w, st->fat);
        wr(&w, ",\"cluster\":%lu,\"size_mb\":%lu,\"restart\":true", (unsigned long)st->cluster,
           (unsigned long)st->size_mb);
    } else if (st->phase == GW_CARDFMT_FAILED) {
        wr(&w, ",\"duration_s\":%lu,\"error\":", (unsigned long)s);
        wr_str(&w, st->why);
    }
    wr(&w, "}");
    return json_end(&w);
}
