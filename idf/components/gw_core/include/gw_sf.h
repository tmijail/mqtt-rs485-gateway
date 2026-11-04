/*
 * Store & forward buffer (SPB-14): a bounded ring of metric samples captured while there is
 * no Sparkplug session (broker down, primary host offline), published later as historical
 * data (is_historical = true).
 *
 * Pure C, no allocation: the caller hands in the storage (allocated once at boot, in PSRAM
 * when present). Samples are grouped in batches: one batch is the set of changes one
 * publication would have carried (a DDATA of one device, or an NDATA), and is published back
 * as one message. When the ring is full the oldest samples are overwritten (the newest data
 * is kept) and counted as dropped.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_SF_NODE 0xFFu   /* gw_sf_rec_t.dev of a node metric (local DI/DO, NDATA) */

typedef struct {
    uint64_t ts_ms;        /* sample time (metric timestamp) */
    double value;
    uint32_t batch;        /* samples of one batch become one message */
    uint16_t metric;       /* device tag index, or the node metric alias for GW_SF_NODE */
    uint8_t dev;           /* device index or GW_SF_NODE */
    uint8_t is_null;
} gw_sf_rec_t;

typedef struct {
    gw_sf_rec_t *rec;
    size_t cap;
    size_t head;           /* next slot written */
    size_t count;
    uint32_t next_batch;
    uint32_t dropped;      /* samples overwritten before they could be sent */
    uint32_t stored;       /* samples ever stored */
} gw_sf_t;

void gw_sf_init(gw_sf_t *sf, gw_sf_rec_t *storage, size_t cap);

/** Start a batch; the samples pushed until the next call share its id. */
void gw_sf_begin(gw_sf_t *sf);

/** Append a sample to the current batch (overwrites the oldest one when full). */
void gw_sf_push(gw_sf_t *sf, uint8_t dev, uint16_t metric, double value, bool is_null,
                uint64_t ts_ms);

/** The oldest batch, at most @p max samples of it, copied to @p out (oldest first); 0 if empty.
 *  Only samples of one batch and one device are returned together. */
size_t gw_sf_peek(const gw_sf_t *sf, gw_sf_rec_t *out, size_t max);

/** Remove the @p n oldest samples (after they were published). */
void gw_sf_pop(gw_sf_t *sf, size_t n);

static inline size_t gw_sf_count(const gw_sf_t *sf)
{
    return sf->count;
}

#ifdef __cplusplus
}
#endif
