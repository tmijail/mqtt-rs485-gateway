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
 * With a spill attached (gw_sfspill, microSD), gw_sf_step() moves the oldest records to it, one
 * 4 KiB block per call, once the ring is three quarters full; they are replayed from there
 * first. Pushing never touches the card (#124): a full ring overwrites its oldest samples (the
 * newest data is kept) and counts them as dropped, with or without a spill.
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

/* Records of the longest sample: a string head and 255 / 8 rounded up continuations. */
#define GW_SF_SAMPLE_MAX_RECS 33u

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
    size_t spill_at;       /* records in the ring from which a block moves to the spill */
    uint32_t spilled;      /* records moved to the spill */
    bool peeked_spill;     /* the last peek came from the spill */
} gw_sf_t;

void gw_sf_init(gw_sf_t *sf, gw_sf_rec_t *storage, size_t cap);

/** Extend the ring with a spill (gw_sfspill_t *). */
void gw_sf_attach_spill(gw_sf_t *sf, void *spill);

/**
 * One bounded card operation of the spill (call once per pass of the owner's loop): a step of
 * its recovery or upkeep, or one block of the oldest records when the ring is three quarters
 * full. True if it did one. @p now_ms: a monotonic time.
 */
bool gw_sf_step(gw_sf_t *sf, uint32_t now_ms);

/** True while gw_sf_step() has work to do (call it soon). */
bool gw_sf_busy(const gw_sf_t *sf);

/** Start a batch; the samples pushed until the next call share its id. */
void gw_sf_begin(gw_sf_t *sf);

/** Append a numeric sample to the current batch. */
void gw_sf_push(gw_sf_t *sf, uint8_t dev, uint16_t metric, double value, bool is_null,
                uint64_t ts_ms);

/** Append an Int64/UInt64 sample (its 64 bits) to the current batch. */
void gw_sf_push_u64(gw_sf_t *sf, uint8_t dev, uint16_t metric, uint64_t bits, bool is_null,
                    uint64_t ts_ms);

/** Append @p len raw bytes (SPB-19 Bytes; NUL included, at most 255 kept). */
void gw_sf_push_bytes(gw_sf_t *sf, uint8_t dev, uint16_t metric, const void *data, size_t len,
                      bool is_null, uint64_t ts_ms);

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

/** Records waiting (ring and spill; not those on a card that failed, #134). */
size_t gw_sf_count(const gw_sf_t *sf);

#ifdef __cplusplus
}
#endif
