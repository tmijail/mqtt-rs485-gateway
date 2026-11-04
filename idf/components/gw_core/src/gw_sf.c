#include "gw_sf.h"

#include <string.h>

#include "gw_sfspill.h"

void gw_sf_init(gw_sf_t *sf, gw_sf_rec_t *storage, size_t cap)
{
    memset(sf, 0, sizeof(*sf));
    sf->rec = storage;
    sf->cap = storage != NULL ? cap : 0;
}

void gw_sf_attach_spill(gw_sf_t *sf, void *spill, size_t chunk)
{
    sf->spill = spill;
    sf->spill_chunk = chunk == 0 ? 1 : chunk > sf->cap ? sf->cap : chunk;
}

void gw_sf_begin(gw_sf_t *sf)
{
    sf->next_batch++;
}

static size_t tail(const gw_sf_t *sf)
{
    return (sf->head + sf->cap - sf->count) % sf->cap;
}

static const gw_sf_rec_t *at(const gw_sf_t *sf, size_t i)
{
    return &sf->rec[(tail(sf) + i) % sf->cap];
}

/* Move the oldest records of the ring to the spill (whole string samples only). */
static bool spill_some(gw_sf_t *sf)
{
    gw_sfspill_t *sp = sf->spill;
    if (sp == NULL || !sp->ok || sf->count == 0) {
        return false;
    }
    size_t n = sf->spill_chunk < sf->count ? sf->spill_chunk : sf->count;
    while (n < sf->count && (at(sf, n)->flags & GW_SF_CONT)) {
        n++;   /* never split a string sample */
    }
    size_t t = tail(sf);
    size_t first = n < sf->cap - t ? n : sf->cap - t;   /* contiguous part, then the wrap */
    bool ok = gw_sfspill_append(sp, &sf->rec[t], first) &&
              (first == n || gw_sfspill_append(sp, &sf->rec[0], n - first));
    if (ok) {
        sf->count -= n;
        sf->spilled += (uint32_t)n;
    }
    return ok;
}

static void put(gw_sf_t *sf, const gw_sf_rec_t *r)
{
    if (sf->cap == 0) {
        if (!(r->flags & GW_SF_CONT)) {
            sf->dropped++;
        }
        return;
    }
    if (sf->count == sf->cap && !spill_some(sf)) {
        /* overwrite the oldest record; a string losing its head leaves continuations, which
         * gw_sf_peek() skips */
        if (!(at(sf, 0)->flags & GW_SF_CONT)) {
            sf->dropped++;
        }
        sf->count--;
    }
    sf->rec[sf->head] = *r;
    sf->head = (sf->head + 1) % sf->cap;
    sf->count++;
}

static void push(gw_sf_t *sf, uint8_t dev, uint16_t metric, uint64_t bits, double num,
                 bool is_num, uint8_t flags, uint64_t ts_ms)
{
    gw_sf_rec_t r = {.ts_ms = ts_ms, .batch = sf->next_batch, .metric = metric, .dev = dev,
                     .flags = flags};
    if (is_num) {
        r.v.num = num;
    } else {
        r.v.u64 = bits;
    }
    put(sf, &r);
    if (!(flags & GW_SF_CONT)) {
        sf->stored++;
    }
}

void gw_sf_push(gw_sf_t *sf, uint8_t dev, uint16_t metric, double value, bool is_null,
                uint64_t ts_ms)
{
    push(sf, dev, metric, 0, value, true, is_null ? GW_SF_NULL : 0, ts_ms);
}

void gw_sf_push_u64(gw_sf_t *sf, uint8_t dev, uint16_t metric, uint64_t bits, bool is_null,
                    uint64_t ts_ms)
{
    push(sf, dev, metric, bits, 0, false, is_null ? GW_SF_NULL : 0, ts_ms);
}

void gw_sf_push_str(gw_sf_t *sf, uint8_t dev, uint16_t metric, const char *s, bool is_null,
                    uint64_t ts_ms)
{
    size_t len = is_null || s == NULL ? 0 : strnlen(s, 255);
    push(sf, dev, metric, len, 0, false, (uint8_t)(GW_SF_STR | (is_null ? GW_SF_NULL : 0)),
         ts_ms);
    for (size_t off = 0; off < len; off += 8) {
        gw_sf_rec_t c = {.ts_ms = ts_ms, .batch = sf->next_batch, .metric = metric, .dev = dev,
                         .flags = GW_SF_CONT};
        size_t k = len - off < 8 ? len - off : 8;
        memcpy(c.v.bytes, s + off, k);
        put(sf, &c);
    }
}

size_t gw_sf_sample(const gw_sf_rec_t *recs, size_t n, char *str, size_t cap)
{
    if (n == 0) {
        return 0;
    }
    if (!(recs[0].flags & GW_SF_STR)) {
        return 1;
    }
    size_t len = (size_t)recs[0].v.u64, used = 1, w = 0;
    for (size_t off = 0; off < len && used < n && (recs[used].flags & GW_SF_CONT);
         off += 8, used++) {
        for (size_t k = 0; k < 8 && off + k < len; k++) {
            if (w + 1 < cap) {
                str[w++] = recs[used].v.bytes[k];
            }
        }
    }
    if (cap > 0) {
        str[w] = '\0';
    }
    return used;
}

/* Cut @p n records copied in @p out back to whole samples of the first batch and device. */
static size_t trim(const gw_sf_rec_t *out, size_t n)
{
    size_t i = 0;
    while (i < n) {
        if (out[i].batch != out[0].batch || out[i].dev != out[0].dev ||
            (i > 0 && (out[i].flags & GW_SF_CONT))) {
            break;
        }
        size_t need = 1;
        if (out[i].flags & GW_SF_STR) {
            need += (size_t)((out[i].v.u64 + 7) / 8);
        }
        if (i + need > n) {
            break;   /* the sample does not fit in the window: it goes in the next message */
        }
        i += need;
    }
    return i;
}

size_t gw_sf_peek(gw_sf_t *sf, gw_sf_rec_t *out, size_t max)
{
    gw_sfspill_t *sp = sf->spill;
    sf->peeked_spill = false;
    while (sp != NULL && sp->count > 0) {
        size_t n = gw_sfspill_peek(sp, out, max);
        if (n == 0) {
            break;   /* I/O error: the ring is next */
        }
        size_t skip = 0;
        while (skip < n && (out[skip].flags & GW_SF_CONT)) {
            skip++;   /* continuations whose head was dropped */
        }
        if (skip > 0) {
            gw_sfspill_consume(sp, skip);
            continue;
        }
        sf->peeked_spill = true;
        size_t k = trim(out, n);
        return k > 0 ? k : 1;   /* a sample longer than the window: sent alone */
    }
    while (sf->count > 0 && (at(sf, 0)->flags & GW_SF_CONT)) {
        sf->count--;   /* continuations whose head was overwritten */
    }
    size_t n = 0;
    while (n < max && n < sf->count) {
        out[n] = *at(sf, n);
        n++;
    }
    if (n == 0) {
        return 0;
    }
    size_t k = trim(out, n);
    return k > 0 ? k : 1;
}

void gw_sf_pop(gw_sf_t *sf, size_t n)
{
    if (sf->peeked_spill) {
        gw_sfspill_consume(sf->spill, n);
        return;
    }
    sf->count -= n < sf->count ? n : sf->count;
}

size_t gw_sf_count(const gw_sf_t *sf)
{
    const gw_sfspill_t *sp = sf->spill;
    return sf->count + (sp != NULL ? (size_t)sp->count : 0);
}
