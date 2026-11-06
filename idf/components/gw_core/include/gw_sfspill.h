/*
 * Store & forward spill (SPB-14): an optional, bounded extension of the RAM ring on a card
 * (microSD) or, in the QEMU build, a LittleFS directory. Pure policy; the storage is injected.
 *
 * Format: numbered segment files, append-only. Each starts with a 16-byte header
 * (gw_sfspill_hdr_t: magic, format version, record size, configuration hash, segment id)
 * followed by raw gw_sf_rec_t records. A segment is written once, front to back, and deleted
 * whole once replayed (or when the space is needed): no sector is rewritten in place, so the
 * card's wear levelling only sees sequential writes and file deletions. A trailing partial
 * record (power cut during a write) is ignored.
 *
 * Bounds: at most max_segs segments of seg_records records. When a new segment is needed and
 * there is no room, the oldest segment is deleted and its unreplayed records are counted as
 * dropped (the newest data is kept, as in the RAM ring).
 *
 * Across a reboot: segments whose configuration hash matches are kept and replayed (oldest
 * first, by segment id); any other file is deleted (tag indexes would not mean the same).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gw_sf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_SFSPILL_MAGIC   0x46535747u   /* "GWSF" */
#define GW_SFSPILL_VERSION 1u
#define GW_SFSPILL_MAX_SEGS 128

/* Layout planned by gw_sfspill_plan() for sparkplug.store_forward.sd.max_kb (#119). */
#define GW_SFSPILL_MIN_KB        64        /* bounds of max_kb, also enforced by gw_config */
#define GW_SFSPILL_MAX_KB        4194304   /* 4 GiB: a segment stays below 2^31 bytes */
#define GW_SFSPILL_MIN_SEGS      8         /* segments, at least */
#define GW_SFSPILL_SEG_RECORDS   4096      /* preferred records per segment: bigger only once
                                              GW_SFSPILL_MAX_SEGS segments are in use */

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t rec_size;
    uint32_t cfg_hash;
    uint32_t seg_id;
} gw_sfspill_hdr_t;

/** Storage of numbered segments. Every call returns false (or -1) on an I/O error. */
typedef struct {
    void *ctx;
    /** Append @p len bytes to segment @p seg (created if absent). */
    bool (*append)(void *ctx, uint32_t seg, const void *data, size_t len);
    /** Read @p len bytes at byte @p offset of segment @p seg. */
    bool (*read)(void *ctx, uint32_t seg, uint32_t offset, void *data, size_t len);
    /** Delete segment @p seg (absent is not an error). */
    bool (*remove)(void *ctx, uint32_t seg);
    /** Size in bytes of segment @p seg, -1 if absent. */
    int32_t (*size)(void *ctx, uint32_t seg);
    /** Ids of the existing segments (any order), at most @p max. */
    size_t (*list)(void *ctx, uint32_t *ids, size_t max);
} gw_sfspill_io_t;

typedef struct {
    gw_sfspill_io_t io;
    uint32_t seg_records;
    uint32_t max_segs;
    uint32_t cfg_hash;
    bool ok;                 /* false after an I/O error: the spill is not used any more */
    /* segments [first, last] exist; last is the one being written */
    uint32_t first, last;
    uint32_t nsegs;          /* 0 = none */
    uint32_t next_id;        /* id of the next segment created */
    uint32_t read_off;       /* records already replayed from segment first */
    uint32_t last_count;     /* records in segment last */
    uint32_t first_count;    /* records in segment first (== last_count when first == last) */
    uint64_t count;          /* records not yet replayed */
    uint32_t dropped;        /* records deleted unreplayed (space) */
    uint32_t written;        /* records ever written */
    uint32_t kept_at_boot;   /* records found from before the boot */
} gw_sfspill_t;

/**
 * Segment layout for a spill bounded to @p max_kb KiB (clamped to GW_SFSPILL_MIN_KB ..
 * GW_SFSPILL_MAX_KB): GW_SFSPILL_MIN_SEGS .. GW_SFSPILL_MAX_SEGS segments of about
 * GW_SFSPILL_SEG_RECORDS records, segments made larger when more would not fit in the table.
 * The segments with their headers fit in max_kb, and they use all of it but less than one
 * record per segment.
 */
void gw_sfspill_plan(uint32_t max_kb, uint32_t *seg_records, uint32_t *max_segs);

/** Recover the segments left by a previous run (same @p cfg_hash), delete the others. */
void gw_sfspill_init(gw_sfspill_t *sp, const gw_sfspill_io_t *io, uint32_t seg_records,
                     uint32_t max_segs, uint32_t cfg_hash);

/** Append @p n records (oldest first). False on an I/O error (nothing more is spilled). */
bool gw_sfspill_append(gw_sfspill_t *sp, const gw_sf_rec_t *recs, size_t n);

/** Copy up to @p max of the oldest unreplayed records to @p out (from one segment). */
size_t gw_sfspill_peek(gw_sfspill_t *sp, gw_sf_rec_t *out, size_t max);

/** The @p n oldest records were replayed: drop them (deleting finished segments). */
void gw_sfspill_consume(gw_sfspill_t *sp, size_t n);

/** FNV-1a over @p len bytes, chained from @p h (start with GW_SFSPILL_FNV_INIT). */
#define GW_SFSPILL_FNV_INIT 2166136261u
uint32_t gw_sfspill_fnv(uint32_t h, const void *data, size_t len);

#ifdef __cplusplus
}
#endif
