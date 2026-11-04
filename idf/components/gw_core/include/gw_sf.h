/*
 * Store & forward buffer (SPB-14): a bounded ring of metric samples captured while there is
 * no Sparkplug session (broker down, primary host offline), published later as historical
 * data (is_historical = true).
 *
 * Pure C, no allocation: the caller hands in the storage (allocated once at boot, in PSRAM
 * when present). Samples are grouped in batches: one batch is the set of changes one
 * publication would have carried (a DDATA of one device, or an NDATA), and is published back
 * as one message.
 *
 * Records are 24 bytes. A double, Int64 or UInt64 sample is one record (the value exact, as
 * its 64 bits); a string sample (MB-6) is a head record with its length followed by
 * ceil(length / 8) continuation records with its bytes.
 *
 * When the ring is full: with a spill attached (gw_sfspill, microSD), the oldest records go to
 * it, a chunk at a time, and are replayed from there first; without one (or once it fails),
 * the oldest samples are overwritten (the newest data is kept) and counted as dropped.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_SF_NODE 0xFFu   /* gw_sf_rec_t.dev of a node metric (local DI/DO, NDATA) */

/* gw_sf_rec_t.flags */
#define GW_SF_NULL 0x01u   /* the sample is null (read failed) */
#define GW_SF_STR  0x02u   /* string head: v.u64 = length; continuation records follow */
#define GW_SF_CONT 0x04u   /* 8 bytes of the preceding string head */

typedef struct {
    uint64_t ts_ms;        /* sample time (metric timestamp) */
    union {
        double num;        /* every value up to 32 bits, floats, scaled tags */
        int64_t i64;
        uint64_t u64;
        char bytes[8];
    } v;
    uint32_t batch;        /* samples of one batch become one message */
    uint16_t metric;       /* device tag index, or the node metric alias for GW_SF_NODE */
    uint8_t dev;           /* device index or GW_SF_NODE */
    uint8_t flags;
} gw_sf_rec_t;

typedef struct {
    gw_sf_rec_t *rec;
    size_t cap;
    size_t head;           /* next slot written */
    size_t count;          /* records in the ring */
    uint32_t next_batch;
    uint32_t dropped;      /* samples overwritten before they could be sent */
    uint32_t stored;       /* samples ever stored */
    void *spill;           /* gw_sfspill_t, or NULL */
    size_t spill_chunk;    /* records moved to the spill at a time */
    uint32_t spilled;      /* records moved to the spill */
    bool peeked_spill;     /* the last peek came from the spill */
} gw_sf_t;

void gw_sf_init(gw_sf_t *sf, gw_sf_rec_t *storage, size_t cap);

/** Extend the ring with a spill (gw_sfspill_t *): @p chunk records move at a time. */
void gw_sf_attach_spill(gw_sf_t *sf, void *spill, size_t chunk);

/** Start a batch; the samples pushed until the next call share its id. */
void gw_sf_begin(gw_sf_t *sf);

/** Append a numeric sample to the current batch. */
void gw_sf_push(gw_sf_t *sf, uint8_t dev, uint16_t metric, double value, bool is_null,
                uint64_t ts_ms);

/** Append an Int64/UInt64 sample (its 64 bits) to the current batch. */
void gw_sf_push_u64(gw_sf_t *sf, uint8_t dev, uint16_t metric, uint64_t bits, bool is_null,
                    uint64_t ts_ms);

/** Append a string sample (at most 255 bytes are kept). */
void gw_sf_push_str(gw_sf_t *sf, uint8_t dev, uint16_t metric, const char *s, bool is_null,
                    uint64_t ts_ms);

/**
 * The oldest batch (from the spill first), at most @p max records of it, copied to @p out
 * (oldest first); 0 if empty. Only records of one batch and one device are returned
 * together, and never part of a string sample.
 */
size_t gw_sf_peek(gw_sf_t *sf, gw_sf_rec_t *out, size_t max);

/** Remove the @p n oldest records returned by the last gw_sf_peek() (after publishing). */
void gw_sf_pop(gw_sf_t *sf, size_t n);

/** Records of the sample at @p recs[0] (1, or 1 + continuations for a string); its text, NUL
 *  terminated, into @p str when it is a string. */
size_t gw_sf_sample(const gw_sf_rec_t *recs, size_t n, char *str, size_t cap);

/** Records waiting (ring and spill). */
size_t gw_sf_count(const gw_sf_t *sf);

#ifdef __cplusplus
}
#endif
