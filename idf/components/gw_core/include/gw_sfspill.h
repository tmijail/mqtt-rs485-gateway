/*
 * Store & forward spill (SPB-14): an optional, bounded extension of the RAM ring on a card
 * (microSD) or, in the QEMU build, a LittleFS directory. Pure policy; the storage is injected.
 *
 * Format (version 1, unchanged since #119): numbered segment files, append-only. Each starts
 * with a 16-byte header (gw_sfspill_hdr_t: magic, format version, record size, configuration
 * hash, segment id) followed by raw gw_sf_rec_t records. A trailing partial record (power cut
 * during a write) is ignored, and the next append overwrites it from the record boundary.
 *
 * Bounds: at most max_segs segments of seg_records records, and together at most max_segs full
 * segments of bytes (max_kb). When a new segment is needed and there is no room, the oldest
 * segment leaves (its unreplayed records are counted as dropped: the newest data is kept, as
 * in the RAM ring).
 *
 * Delete (#122), never with a rename (a cut in f_rename can cross-link two FAT files):
 *   1. dead: the magic of the header is overwritten in place with GW_SFSPILL_DEAD (one sector).
 *      This happens when a segment leaves the run (replayed or dropped), so a dead segment is
 *      never replayed again after a reboot;
 *   2. shrink: gw_sfspill_step(), called once per pass of the Sparkplug loop, truncates it by
 *      at most one step (GW_SFSPILL_STEP_UNITS allocation units, 128 FAT clusters) and unlinks
 *      it once at most one step is left. Every call frees at most 128 clusters, so it writes at
 *      most two FAT sectors (and their mirror), the directory entry and FSInfo, on any card.
 * The segment size is limited to one step (gw_sfspill_plan() with the real cluster size), so a
 * new segment needs only the unlink; the steps matter for older, larger segments and other
 * files of the directory.
 *
 * Across a reboot (gw_sfspill_init(), reads only): the newest segments with the same
 * configuration hash are kept and replayed (oldest first, by segment id), as many as fit in
 * max_segs and max_kb; older ones (a smaller max_kb), dead segments and any other file are
 * queued and deleted by gw_sfspill_step(), after the boot, in steps.
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
#define GW_SFSPILL_DEAD    0x44414544u   /* "DEAD": the magic of a segment being deleted */
#define GW_SFSPILL_VERSION 1u
#define GW_SFSPILL_MAX_SEGS 128

/* Layout planned by gw_sfspill_plan() for sparkplug.store_forward.sd.max_kb (#119). */
#define GW_SFSPILL_MIN_KB        64        /* bounds of max_kb, also enforced by gw_config */
#define GW_SFSPILL_MAX_KB        4194304   /* 4 GiB: a segment stays below 2^31 bytes */
#define GW_SFSPILL_MIN_SEGS      8         /* segments, at least */
#define GW_SFSPILL_SEG_RECORDS   4096      /* preferred records per segment: bigger only once
                                              GW_SFSPILL_MAX_SEGS segments are in use */
#define GW_SFSPILL_STEP_UNITS    128       /* #122: allocation units one delete step frees */
#define GW_SFSPILL_LIST_MAX      (2 * GW_SFSPILL_MAX_SEGS)   /* files examined at boot */
#define GW_SFSPILL_ID_LIMIT      0x80000000u   /* higher file numbers are not ours */

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t rec_size;
    uint32_t cfg_hash;
    uint32_t seg_id;
} gw_sfspill_hdr_t;

/**
 * Storage of numbered segments. Every call returns false (or -1) on an I/O error. Each call that
 * changes the storage is durable when it returns (synced). The implementation may keep files
 * open between calls (#122: one handle for the segment being written, one for reads, one for
 * deletes); the policy never has two operations in flight.
 */
typedef struct {
    void *ctx;
    /** Allocation unit (FAT cluster) in bytes; 0 if unknown (no limit on a delete step). */
    uint32_t unit;
    /** Write @p len bytes at byte @p offset (at most its size) of the newest segment @p seg,
     *  created if absent. */
    bool (*write)(void *ctx, uint32_t seg, uint32_t offset, const void *data, size_t len);
    /** Overwrite @p len bytes in place at @p offset of the existing segment @p seg (never
     *  extends it): the dead mark. */
    bool (*patch)(void *ctx, uint32_t seg, uint32_t offset, const void *data, size_t len);
    /** Read @p len bytes at byte @p offset of segment @p seg. */
    bool (*read)(void *ctx, uint32_t seg, uint32_t offset, void *data, size_t len);
    /** Shrink segment @p seg to @p len bytes (less than its size). */
    bool (*truncate)(void *ctx, uint32_t seg, uint32_t len);
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
    /* the run: live segments from first to last (ids in between that are not live, such as a
     * torn segment left by a power cut, are skipped); last is the one being written */
    uint32_t first, last;
    uint32_t nsegs;          /* live segments; 0 = none */
    uint32_t next_id;        /* id of the next segment created */
    uint32_t read_off;       /* records already replayed from segment first */
    uint32_t last_count;     /* records in segment last */
    uint32_t first_count;    /* records in segment first (== last_count when first == last) */
    uint64_t count;          /* records not yet replayed */
    uint64_t bytes;          /* bytes of the live segments */
    uint64_t max_bytes;      /* their bound: max_segs full segments (within max_kb) */
    uint32_t step_bytes;     /* most bytes one delete step frees; 0 = no limit */
    uint32_t dropped;        /* records deleted unreplayed (space) */
    uint32_t written;        /* records ever written */
    uint32_t kept_at_boot;   /* records found from before the boot */
    uint32_t trimmed;        /* records of this configuration deleted at boot (over max_kb) */
    /* the deletes (#122): a queue of segment ids in the caller's storage (GC_MARKED: already
     * dead), and the one in progress */
    uint32_t *gc;
    uint32_t gc_cap, gc_head, gc_n;
    uint32_t gc_seg;
    bool gc_busy;
    bool gc_marked;
    uint32_t gc_files;       /* files deleted */
    uint32_t gc_steps;       /* delete steps taken (mark, truncate or unlink) */
    uint32_t gc_left;        /* deletes left for the next boot (queue full, or an I/O error) */
    /** Called after each bounded card operation (the Sparkplug task feeds its watchdog). */
    void (*tick)(void);
} gw_sfspill_t;

#define GW_SFSPILL_GC_MARKED 0x80000000u

/**
 * Segment layout for a spill bounded to @p max_kb KiB (clamped to GW_SFSPILL_MIN_KB ..
 * GW_SFSPILL_MAX_KB): GW_SFSPILL_MIN_SEGS .. GW_SFSPILL_MAX_SEGS segments of about
 * GW_SFSPILL_SEG_RECORDS records, segments made larger when more would not fit in the table.
 * The segments with their headers fit in max_kb, and they use all of it but less than one
 * record per segment.
 *
 * #122: with an allocation unit @p unit (bytes, 0 = no limit) a segment holds at most
 * GW_SFSPILL_STEP_UNITS units, so that its delete is one bounded step: more, smaller segments.
 * When max_kb needs more than GW_SFSPILL_MAX_SEGS of those, the spill uses less: the return
 * value is the KiB the layout can hold (max_kb when it is not limited).
 */
uint32_t gw_sfspill_plan(uint32_t max_kb, uint32_t unit, uint32_t *seg_records,
                         uint32_t *max_segs);

/**
 * Recover the segments left by a previous run (same @p cfg_hash), within @p max_segs segments
 * of @p seg_records records. Reads only: everything else on the storage is queued for
 * gw_sfspill_step(). @p work (@p work_cap ids, GW_SFSPILL_LIST_MAX is enough for any storage
 * this spill wrote) holds the listing and then the delete queue, for the life of @p sp.
 */
void gw_sfspill_init(gw_sfspill_t *sp, const gw_sfspill_io_t *io, uint32_t seg_records,
                     uint32_t max_segs, uint32_t cfg_hash, uint32_t *work, size_t work_cap);

/** Append @p n records (oldest first). False on an I/O error (nothing more is spilled). */
bool gw_sfspill_append(gw_sfspill_t *sp, const gw_sf_rec_t *recs, size_t n);

/** Copy up to @p max of the oldest unreplayed records to @p out (from one segment). */
size_t gw_sfspill_peek(gw_sfspill_t *sp, gw_sf_rec_t *out, size_t max);

/** The @p n oldest records were replayed: drop them (finished segments are marked dead and
 *  queued for deletion). */
void gw_sfspill_consume(gw_sfspill_t *sp, size_t n);

/** Files waiting to be deleted (queued or in progress). */
uint32_t gw_sfspill_gc_pending(const gw_sfspill_t *sp);

/**
 * One bounded step of the deletes: mark, truncate by at most step_bytes, or unlink a file of at
 * most step_bytes. True if it did one (call again on the next loop pass), false if there is
 * nothing to delete.
 */
bool gw_sfspill_step(gw_sfspill_t *sp);

/** FNV-1a over @p len bytes, chained from @p h (start with GW_SFSPILL_FNV_INIT). */
#define GW_SFSPILL_FNV_INIT 2166136261u
uint32_t gw_sfspill_fnv(uint32_t h, const void *data, size_t len);

#ifdef __cplusplus
}
#endif
