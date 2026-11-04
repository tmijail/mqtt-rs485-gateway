#include "gw_phost.h"

#include <string.h>

void gw_phost_init(gw_phost_t *p)
{
    memset(p, 0, sizeof(*p));
}

void gw_phost_session_start(gw_phost_t *p)
{
    p->online = false;
}

typedef struct {
    const uint8_t *s;
    const uint8_t *end;
} cur_t;

static void ws(cur_t *c)
{
    while (c->s < c->end && (*c->s == ' ' || *c->s == '\t' || *c->s == '\r' || *c->s == '\n')) {
        c->s++;
    }
}

static bool eat(cur_t *c, char ch)
{
    ws(c);
    if (c->s < c->end && *c->s == (uint8_t)ch) {
        c->s++;
        return true;
    }
    return false;
}

static bool lit(cur_t *c, const char *word)
{
    size_t n = strlen(word);
    if ((size_t)(c->end - c->s) < n || memcmp(c->s, word, n) != 0) {
        return false;
    }
    c->s += n;
    return true;
}

/* A JSON string; its content (escapes left as they are) in [*b, *e). */
static bool string(cur_t *c, const uint8_t **b, const uint8_t **e)
{
    if (!eat(c, '"')) {
        return false;
    }
    *b = c->s;
    while (c->s < c->end && *c->s != '"') {
        if (*c->s < 0x20) {
            return false;
        }
        if (*c->s == '\\') {
            c->s++;
            if (c->s >= c->end) {
                return false;
            }
        }
        c->s++;
    }
    if (c->s >= c->end) {
        return false;
    }
    *e = c->s++;
    return true;
}

/* Any JSON value, skipped (nesting bounded: STATE payloads are flat). */
static bool skip_value(cur_t *c, int depth)
{
    ws(c);
    if (c->s >= c->end || depth > 4) {
        return false;
    }
    const uint8_t *b, *e;
    switch (*c->s) {
    case '"':
        return string(c, &b, &e);
    case '{':
    case '[': {
        char close = *c->s == '{' ? '}' : ']';
        bool obj = *c->s == '{';
        c->s++;
        if (eat(c, close)) {
            return true;
        }
        do {
            if (obj && (!string(c, &b, &e) || !eat(c, ':'))) {
                return false;
            }
            if (!skip_value(c, depth + 1)) {
                return false;
            }
        } while (eat(c, ','));
        return eat(c, close);
    }
    case 't':
        return lit(c, "true");
    case 'f':
        return lit(c, "false");
    case 'n':
        return lit(c, "null");
    default: {
        const uint8_t *start = c->s;
        while (c->s < c->end && *c->s != '\0' &&
               (strchr("+-.eE", *c->s) != NULL || (*c->s >= '0' && *c->s <= '9'))) {
            c->s++;
        }
        return c->s > start;
    }
    }
}

bool gw_phost_parse(const uint8_t *payload, size_t len, bool *online, uint64_t *timestamp)
{
    cur_t c = {payload, payload + len};
    bool have_online = false, have_ts = false;
    if (!eat(&c, '{')) {
        return false;
    }
    if (!eat(&c, '}')) {
        do {
            const uint8_t *b, *e;
            if (!string(&c, &b, &e) || !eat(&c, ':')) {
                return false;
            }
            size_t klen = (size_t)(e - b);
            ws(&c);
            if (klen == 6 && memcmp(b, "online", 6) == 0) {
                if (have_online) {
                    return false;   /* duplicated key: ambiguous */
                }
                have_online = true;
                if (lit(&c, "true")) {
                    *online = true;
                } else if (lit(&c, "false")) {
                    *online = false;
                } else {
                    return false;
                }
            } else if (klen == 9 && memcmp(b, "timestamp", 9) == 0) {
                if (have_ts) {
                    return false;
                }
                have_ts = true;
                uint64_t v = 0;
                const uint8_t *start = c.s;
                while (c.s < c.end && *c.s >= '0' && *c.s <= '9') {
                    uint64_t d = (uint64_t)(*c.s - '0');
                    if (v > (UINT64_MAX - d) / 10) {
                        return false;
                    }
                    v = v * 10 + d;
                    c.s++;
                }
                if (c.s == start || (c.s < c.end && strchr(".eE", *c.s) != NULL)) {
                    return false;   /* not an unsigned integer */
                }
                *timestamp = v;
            } else if (!skip_value(&c, 0)) {
                return false;
            }
        } while (eat(&c, ','));
        if (!eat(&c, '}')) {
            return false;
        }
    }
    ws(&c);
    return c.s == c.end && have_online && have_ts;
}

gw_phost_result_t gw_phost_on_state(gw_phost_t *p, const uint8_t *payload, size_t len)
{
    bool online;
    uint64_t ts;
    if (!gw_phost_parse(payload, len, &online, &ts)) {
        return GW_PHOST_INVALID;
    }
    if (p->have_ts && ts < p->last_ts) {
        return GW_PHOST_STALE;
    }
    p->have_ts = true;
    p->last_ts = ts;
    p->online = online;
    return online ? GW_PHOST_ONLINE : GW_PHOST_OFFLINE;
}
