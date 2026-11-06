/*
 * Store & forward spill (SPB-14, #124): an optional extension of the RAM ring on a microSD card
 * or, in the QEMU build, a LittleFS directory. Pure policy; the storage is injected (gw_sfspill_io_t).
 *
 * Format (version 2, #124): a crash-safe ring of self-checking 4 KiB blocks.
 *  - The ring is N blocks in E extent files r%04x.sf of M blocks each. An extent is
 *    min(4 MiB, 128 clusters), so creating or deleting one changes at most 128 FAT clusters. An
 *    extent is allocated whole at its first use (one bounded step) and never resized after that.
 *  - A block (gw_sfspill_blk_t + up to GW_SFSPILL_BLK_RECS records) is written once, whole and
 *    aligned, with no fsync: no FAT or directory write in normal operation. It is VALID only if
 *    its CRC-32C is right, its nonce is the ring's nonce and seq mod N is its position: data of
 *    an earlier ring, of an earlier lap or damaged by a power cut is never replayed.
 *  - Two superblock copies (gw_sfspill_sb_t) in two files, ringa.sf and ringb.sf, hold the
 *    geometry, the nonce, the replay checkpoint (tail) and a head hint. The copy with the lower
 *    generation is written, never both one after the other (GW_SFSPILL_SB_GAP_MS), and a new
 *    superblock (new nonce or geometry) is written before any block that uses it.
 *  - Checkpoint: the tail moves on the card only after the publish of those records returned,
 *    and the replay stops until a checkpoint once GW_SFSPILL_CKPT_RECS records (64 KiB) were
 *    replayed since the last one: a reboot during a replay sends at most 64 KiB again.
 *  - Boot recovery, in bounded steps after the network starts (gw_sfspill_prepare()): the newest
 *    valid superblock, then a forward scan from its head hint that skips up to
 *    GW_SFSPILL_HOLES_MAX invalid blocks as holes (counted as lost, never replayed); with no valid
 *    superblock, the nonce comes from the first block of each extent (a vote) and the head from a
 *    scan after the newest of them.
 *  - A configuration (devices, tags) or geometry (max_kb, cluster size) change writes a new
 *    superblock with a new nonce: the old blocks are invalid at once, no file is deleted for it.
 *    Extents beyond the new ring are deleted one per step.
 *  - Card full while the ring is allocated the first time: the ring stops at the extents it
 *    has (a new superblock, the same nonce: every block written so far stays valid).
 *
 * Migration (format version 1, step 1, #119/#122): numbered segment files NNNNNNNN.sf with a
 * 16-byte header and raw records, no CRC. At boot those of this configuration are replayed once
 * (before the ring, oldest first; the superblock keeps the position, so a reboot repeats at most
 * 64 KiB of them too) and then deleted; the others (another configuration, dead, torn) are
 * deleted unreplayed. A delete is: the magic overwritten with GW_SFSPILL_DEAD in place (never a
 * rename), then truncates of at most 128 clusters, then the unlink, one per step.
 *
 * Real time: the card work happens in gw_sfspill_prepare()/gw_sfspill_put_block() (one bounded
 * operation per call, at most a few small writes; the caller calls them once per pass of its
 * loop) and in gw_sfspill_peek() (block reads, at most GW_SFSPILL_SCAN_READS per call). @tick is
 * called after every card operation (the Sparkplug task feeds its watchdog). Nothing allocates:
 * the buffers come from the caller, once (gw_sfspill_mem_size()).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gw_sf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- format version 2: the ring ---- */
#define GW_SFSPILL_BLOCK        4096u        /* bytes of a block, aligned to 4 KiB in its extent */
#define GW_SFSPILL_BLK_MAGIC    0x42575347u  /* "GSWB" */
#define GW_SFSPILL_SB_MAGIC     0x53575347u  /* "GSWS" */
#define GW_SFSPILL_VERSION2     2u
#define GW_SFSPILL_BLK_RECS     169u         /* (4096 - 32) / 24 records in a block */
#define GW_SFSPILL_SB_BYTES     512u         /* one sector: the superblock image */
#define GW_SFSPILL_EXT_MAX      (4u * 1024u * 1024u)   /* an extent is at most 4 MiB ... */
#define GW_SFSPILL_STEP_UNITS   128          /* ... and 128 allocation units (FAT clusters) */
/* At most 1024 extents: 4 GiB with 32 KiB clusters (the max_kb maximum), 512 MiB with 4 KiB
 * clusters (the 128 MiB default needs 256 extents of 512 KiB). The bound keeps a directory
 * lookup at most 64 sector reads (1024 8.3 entries of 32 B) and the boot listing short; more
 * extents of smaller clusters would also make more, smaller FAT steps for the same data. */
#define GW_SFSPILL_MAX_EXTENTS  1024
/* K: invalid blocks skipped as holes in the boot scan (64 blocks = 256 KiB). Paired-page damage
 * on TLC NAND can reach pages programmed several pages before the cut, and a 4 KiB block is
 * smaller than a 16 KiB NAND page; a block read costs about 1-3 ms, so 64 reads past the end at
 * each boot cost well under 1 s. A larger K cannot accept wrong data: a block after a hole must
 * still have the expected seq. */
#define GW_SFSPILL_HOLES_MAX    64
/* H: the head hint is written every 32 blocks (128 KiB) while spilling: the boot scan reads at
 * most H + K blocks after a valid superblock. */
#define GW_SFSPILL_HINT_BLOCKS  32
/* The replay checkpoint: at most 64 KiB of records replayed between two (2730 samples). */
#define GW_SFSPILL_CKPT_RECS    (65536u / 24u)
/* Never write the two superblock copies closer than this (ms): their protection is the time
 * between them, not their place on the card. */
#define GW_SFSPILL_SB_GAP_MS    500u
/* Card reads in one boot step, and in one gw_sfspill_peek() over damaged blocks. */
#define GW_SFSPILL_SCAN_READS   8

/* max_kb (sparkplug.store_forward.sd.max_kb) */
#define GW_SFSPILL_MIN_KB       64
#define GW_SFSPILL_MAX_KB       4194304      /* 4 GiB */
#define GW_SFSPILL_DEFAULT_KB   131072       /* 128 MiB (#124) */

/* File ids of the storage: a step 1 segment is its number (below 0x80000000); an extent and a
 * superblock copy carry these marks. */
#define GW_SFSPILL_F_EXTENT     0x80000000u  /* | extent index: r%04x.sf */
#define GW_SFSPILL_F_SUPER      0xC0000000u  /* | 0 (ringa.sf) or 1 (ringb.sf) */
#define GW_SFSPILL_F_KIND       0xC0000000u
#define GW_SFSPILL_ID_LIMIT     0x80000000u  /* step 1 segment numbers are below */

typedef struct {
    uint32_t magic;          /* GW_SFSPILL_BLK_MAGIC */
    uint16_t version;        /* GW_SFSPILL_VERSION2 */
    uint16_t rec_size;       /* sizeof(gw_sf_rec_t) */
    uint32_t nonce;          /* of the ring instance */
    uint32_t cfg_hash;       /* devices and tags */
    uint64_t seq;            /* block number since the ring began; position = seq mod N */
    uint16_t nrec;           /* 1 .. GW_SFSPILL_BLK_RECS */
    uint16_t flags;          /* 0 */
    uint32_t crc;            /* CRC-32C of this header (crc = 0) and the nrec records */
} gw_sfspill_blk_t;

typedef struct {
    uint32_t magic;          /* GW_SFSPILL_SB_MAGIC */
    uint16_t version;        /* GW_SFSPILL_VERSION2 */
    uint16_t rec_size;
    uint32_t gen;            /* the newer copy has the higher generation */
    uint32_t nonce;
    uint32_t cfg_hash;
    uint32_t ext_blocks;     /* M: blocks of an extent */
    uint32_t extents;        /* E: extents planned (from max_kb and the cluster size) */
    uint32_t nblocks;        /* N: blocks of the ring (E * M, fewer after a full card) */
    uint64_t tail_seq;       /* checkpoint: the first block not fully replayed */
    uint64_t head_hint;      /* the next block to write when this copy was written */
    uint32_t tail_rec;       /* records of block tail_seq already replayed */
    uint32_t count;          /* records waiting between the checkpoint and head_hint */
    uint32_t mig_seg;        /* step 1 segment being replayed: lower numbers are done */
    uint32_t mig_off;        /* its records already replayed */
    uint32_t pad;
    uint32_t crc;            /* CRC-32C of the bytes before it */
} gw_sfspill_sb_t;

/* format version 1 (step 1): read and deleted only */
#define GW_SFSPILL_MAGIC   0x46535747u   /* "GWSF" */
#define GW_SFSPILL_DEAD    0x44414544u   /* "DEAD": a segment being deleted */
#define GW_SFSPILL_VERSION 1u
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t rec_size;
    uint32_t cfg_hash;
    uint32_t seg_id;
} gw_sfspill_hdr_t;
#define GW_SFSPILL_LEGACY_MAX   256          /* step 1 segments examined at boot */
#define GW_SFSPILL_GC_MAX       512          /* files in the delete queue */

/**
 * The storage: files named by a file id (see GW_SFSPILL_F_*). Every call returns false (or -1)
 * on an I/O error. Each call that changes the storage is durable when it returns. The
 * implementation keeps files open between calls; the policy never has two operations in flight.
 */
typedef struct {
    void *ctx;
    /** Allocation unit (FAT cluster) in bytes; 0 if unknown. */
    uint32_t unit;
    /** Make file @p f at least @p size bytes long, created if absent, its space allocated and its
     *  size durable (FAT: lseek past the end and fsync; it does not write the data). 1: done,
     *  0: no space on the card (the file may be partly extended), -1: I/O error. */
    int (*alloc)(void *ctx, uint32_t f, uint32_t size);
    /** Overwrite @p len bytes at @p offset, inside the size of file @p f (it never extends it). A
     *  block or a superblock is whole aligned sectors: FAT writes them straight to the card. */
    bool (*write)(void *ctx, uint32_t f, uint32_t offset, const void *data, size_t len);
    /** Read @p len bytes at @p offset of file @p f: 1 done, 0 the file is absent or shorter,
     *  -1 I/O error. */
    int (*read)(void *ctx, uint32_t f, uint32_t offset, void *data, size_t len);
    /** Shrink file @p f to @p len bytes (less than its size). */
    bool (*truncate)(void *ctx, uint32_t f, uint32_t len);
    /** Delete file @p f (absent is not an error). */
    bool (*remove)(void *ctx, uint32_t f);
    /** Size in bytes of file @p f, -1 if absent. */
    int32_t (*size)(void *ctx, uint32_t f);
    /** Call @p each for every file of the directory that is one of ours (any order). False on
     *  an I/O error. */
    bool (*list)(void *ctx, void (*each)(void *arg, uint32_t f), void *arg);
} gw_sfspill_io_t;

/* gw_sfspill_t.phase */
enum {
    GW_SFSPILL_BOOT_SUPER,   /* read the superblock copies */
    GW_SFSPILL_BOOT_LIST,    /* list the directory */
    GW_SFSPILL_BOOT_LEGACY,  /* examine the step 1 segments */
    GW_SFSPILL_BOOT_PROBE,   /* no valid superblock: the first block of each extent */
    GW_SFSPILL_BOOT_SCAN,    /* the head: forward from the hint */
    GW_SFSPILL_READY,
};

typedef struct {
    gw_sfspill_io_t io;
    void (*tick)(void);          /* after each card operation (feeds the watchdog) */
    uint32_t (*random)(void);    /* a new nonce */
    uint32_t cfg_hash;
    /* geometry */
    uint32_t ext_blocks;         /* M */
    uint32_t extents;            /* E planned */
    uint32_t nblocks;            /* N in use */
    /* state */
    int phase;
    bool ok;                     /* false after an I/O error: the spill is not used any more */
    char why[48];                /* why it failed */
    uint32_t nonce;
    uint32_t gen;                /* of the newest superblock copy */
    uint8_t sb_next;             /* the copy written next (0 = A, 1 = B) */
    uint8_t sb_alloc;            /* bit c: copy c has its file (this boot) */
    bool sb_valid[2];
    gw_sfspill_sb_t sb[2];       /* the copies read at boot */
    bool sb_due;                 /* a new geometry or nonce: a superblock before any block */
    bool sb_written;             /* one was written since boot */
    uint32_t sb_last_ms;         /* time of the last superblock write */
    uint64_t head, tail;         /* blocks [tail, head) wait (head: the next written) */
    uint32_t tail_rec;           /* records of block tail already replayed */
    uint64_t ring_count;         /* records waiting in the ring (exact unless blocks were lost
                                    since the last superblock: then an estimate until empty) */
    uint64_t hint_at;            /* head at the last superblock write */
    uint32_t ck_pending;         /* records replayed since the last superblock write */
    uint32_t ck_max;             /* the most of them at any time (duplicates bound, shown) */
    uint8_t *wbuf, *rbuf;        /* 4 KiB each, from gw_sfspill_mem_size() */
    uint64_t rseq;               /* block held in rbuf, when rvalid */
    bool rvalid;
    uint8_t ext_ok[GW_SFSPILL_MAX_EXTENTS / 8];   /* extents known to be whole (this boot) */
    int peek_src;                /* 0 none, 1 step 1 segment, 2 ring */
    /* boot scan */
    uint64_t scan_seq;           /* next block examined */
    uint64_t scan_end;           /* no valid block at or after this */
    uint32_t scan_run;           /* invalid blocks since the last valid one */
    uint64_t scan_recs;          /* records of the valid blocks after the hint */
    uint32_t probe_e;            /* next extent probed */
    uint32_t vote_nonce[4], vote_n[4];
    uint64_t vote_seq[4];
    uint32_t boot_start_ms, boot_ms;   /* the boot recovery: when it began, how long it took */
    uint32_t scan_reads;         /* blocks read by the boot recovery */
    /* step 1 segments */
    uint32_t *leg;               /* GW_SFSPILL_LEGACY_MAX: listed, then the ones to replay */
    uint32_t leg_n, leg_i, leg_seen;
    uint32_t leg_off, leg_recs;  /* records replayed and in segment leg[leg_i] */
    uint64_t leg_count;          /* records of step 1 segments waiting */
    uint32_t mig_seg, mig_off;   /* the superblock's position in them */
    /* the deletes: a queue of file ids (bit 32: already marked dead) */
    uint64_t *gc;
    uint32_t gc_head, gc_n;
    uint32_t gc_f;
    bool gc_busy, gc_marked;
    uint32_t gc_files, gc_steps, gc_left;
    /* counters */
    uint64_t written;            /* records written to the card */
    uint32_t blocks;             /* blocks written */
    uint32_t dropped;            /* records overwritten before their replay (card full) */
    uint32_t holes;              /* invalid blocks met inside the data (lost, never replayed) */
    uint64_t stranded;           /* records on the card when the spill failed (#134) */
    uint32_t sb_writes, checkpoints, allocs;
    uint32_t legacy_found;       /* step 1 records found at boot (of this configuration) */
    uint32_t kept_at_boot;       /* ring records found at boot */
} gw_sfspill_t;

/* gw_sfspill_prepare() results */
enum { GW_SFSPILL_IDLE, GW_SFSPILL_DID, GW_SFSPILL_BLOCK_READY };

/**
 * Geometry for max_kb KiB (clamped to GW_SFSPILL_MIN_KB .. GW_SFSPILL_MAX_KB) on a card with
 * allocation unit @p unit bytes (0: unknown): extents of min(4 MiB, 128 units) (whole blocks; a
 * smaller max_kb makes one smaller extent), at most GW_SFSPILL_MAX_EXTENTS. Returns the KiB the
 * ring uses: max_kb rounded down to whole extents, less when the extent count limits it.
 */
uint32_t gw_sfspill_plan(uint32_t max_kb, uint32_t unit, uint32_t *ext_blocks, uint32_t *extents);

/** Bytes of memory gw_sfspill_init() needs (two blocks, the step 1 list, the delete queue). */
size_t gw_sfspill_mem_size(void);

/**
 * Start a spill on @p io with the geometry of gw_sfspill_plan() and the configuration hash
 * @p cfg_hash. @p mem (gw_sfspill_mem_size() bytes, 8-byte aligned) is used for the life of
 * @p sp. Does no I/O: the recovery is done by gw_sfspill_prepare(), in steps.
 */
void gw_sfspill_init(gw_sfspill_t *sp, const gw_sfspill_io_t *io, uint32_t ext_blocks,
                     uint32_t extents, uint32_t cfg_hash, void *mem);

/**
 * One bounded card operation, if one is due: a step of the boot recovery, a superblock
 * (geometry, checkpoint or head hint), an extent allocation (when @p want_block and the next
 * block's extent is not known to be whole), or a delete step. GW_SFSPILL_BLOCK_READY: nothing
 * was done and gw_sfspill_put_block() can write the next block now (only if @p want_block).
 * @p now_ms: a monotonic time.
 */
int gw_sfspill_prepare(gw_sfspill_t *sp, bool want_block, uint32_t now_ms);

/** Where the records of the next block go (GW_SFSPILL_BLK_RECS of room). */
gw_sf_rec_t *gw_sfspill_block_recs(gw_sfspill_t *sp);

/** Write the next block with @p n records (1 .. GW_SFSPILL_BLK_RECS) from
 *  gw_sfspill_block_recs(), after GW_SFSPILL_BLOCK_READY. False on an I/O error. */
bool gw_sfspill_put_block(gw_sfspill_t *sp, size_t n);

/** True while gw_sfspill_prepare() has work (recovery, a due superblock, deletes). */
bool gw_sfspill_busy(const gw_sfspill_t *sp);

/** Records waiting on the card (0 after a failure; at least 1 while a block waits). */
uint64_t gw_sfspill_waiting(const gw_sfspill_t *sp);

/** True while the replay must not go past the spill: it may have data that is not readable
 *  yet (the recovery runs) or now (a checkpoint is due). */
bool gw_sfspill_holds(const gw_sfspill_t *sp);

/**
 * Copy up to @p max of the oldest records to @p out (from one step 1 segment or one block): 0
 * when there is none now (recovery, checkpoint due, damaged blocks skipped: try again) or after
 * an I/O error (sp->ok false).
 */
size_t gw_sfspill_peek(gw_sfspill_t *sp, gw_sf_rec_t *out, size_t max);

/** The @p n oldest records (of the last peek) were published: drop them. */
void gw_sfspill_consume(gw_sfspill_t *sp, size_t n);

/** Before a planned restart: write the replay position now if it moved (one superblock, unless
 *  one was written less than GW_SFSPILL_SB_GAP_MS ago). True if it wrote one. */
bool gw_sfspill_checkpoint(gw_sfspill_t *sp, uint32_t now_ms);

/** Stop using the card (an operator formats it): as after an I/O error, with @p why. */
void gw_sfspill_stop(gw_sfspill_t *sp, const char *why);

/** Files waiting to be deleted (queued or in progress). */
uint32_t gw_sfspill_gc_pending(const gw_sfspill_t *sp);

/** CRC-32C (Castagnoli), chained from @p crc (start with 0). */
uint32_t gw_sfspill_crc32c(uint32_t crc, const void *data, size_t len);

/** FNV-1a over @p len bytes, chained from @p h (start with GW_SFSPILL_FNV_INIT). */
#define GW_SFSPILL_FNV_INIT 2166136261u
uint32_t gw_sfspill_fnv(uint32_t h, const void *data, size_t len);

#ifdef __cplusplus
}
#endif
