#include "gw_sf.h"

#include <string.h>

void gw_sf_init(gw_sf_t *sf, gw_sf_rec_t *storage, size_t cap)
{
    memset(sf, 0, sizeof(*sf));
    sf->rec = storage;
    sf->cap = storage != NULL ? cap : 0;
}

void gw_sf_begin(gw_sf_t *sf)
{
    sf->next_batch++;
}

void gw_sf_push(gw_sf_t *sf, uint8_t dev, uint16_t metric, double value, bool is_null,
                uint64_t ts_ms)
{
    if (sf->cap == 0) {
        sf->dropped++;
        return;
    }
    gw_sf_rec_t *r = &sf->rec[sf->head];
    r->ts_ms = ts_ms;
    r->value = value;
    r->batch = sf->next_batch;
    r->metric = metric;
    r->dev = dev;
    r->is_null = is_null ? 1u : 0u;
    sf->head = (sf->head + 1) % sf->cap;
    if (sf->count == sf->cap) {
        sf->dropped++;   /* the oldest sample was overwritten */
    } else {
        sf->count++;
    }
    sf->stored++;
}

static size_t tail(const gw_sf_t *sf)
{
    return (sf->head + sf->cap - sf->count) % sf->cap;
}

size_t gw_sf_peek(const gw_sf_t *sf, gw_sf_rec_t *out, size_t max)
{
    size_t n = 0;
    if (sf->count == 0) {
        return 0;
    }
    size_t i = tail(sf);
    const gw_sf_rec_t *first = &sf->rec[i];
    while (n < max && n < sf->count) {
        const gw_sf_rec_t *r = &sf->rec[(i + n) % sf->cap];
        if (r->batch != first->batch || r->dev != first->dev) {
            break;
        }
        out[n++] = *r;
    }
    return n;
}

void gw_sf_pop(gw_sf_t *sf, size_t n)
{
    sf->count -= n < sf->count ? n : sf->count;
}
