/* QA-1: store & forward ring (SPB-14). The node-level behaviour (historical DDATA/NDATA after
 * the births) is in test_node. */
#include <stdio.h>
#include <string.h>

#include "gw_sf.h"
#include "gw_sfspill.h"
#include "unity.h"

static gw_sf_rec_t storage[8];
static gw_sf_t sf;

void setUp(void)
{
    gw_sf_init(&sf, storage, 8);
}

void tearDown(void) {}

static void test_batches_come_out_in_order(void)
{
    gw_sf_rec_t out[8];
    TEST_ASSERT_EQUAL_size_t(0, gw_sf_peek(&sf, out, 8));
    gw_sf_begin(&sf);
    gw_sf_push(&sf, 0, 1, 10.0, false, 1000);
    gw_sf_push(&sf, 0, 2, 20.0, false, 1000);
    gw_sf_begin(&sf);
    gw_sf_push(&sf, GW_SF_NODE, 10, 1.0, false, 1100);
    gw_sf_begin(&sf);
    gw_sf_push(&sf, 0, 1, 11.0, true, 1200);
    TEST_ASSERT_EQUAL_size_t(4, gw_sf_count(&sf));

    TEST_ASSERT_EQUAL_size_t(2, gw_sf_peek(&sf, out, 8));
    TEST_ASSERT_EQUAL_UINT16(1, out[0].metric);
    TEST_ASSERT_EQUAL_DOUBLE(20.0, out[1].v.num);
    TEST_ASSERT_EQUAL_UINT64(1000, out[1].ts_ms);
    /* a batch larger than the caller's room comes out in pieces */
    TEST_ASSERT_EQUAL_size_t(1, gw_sf_peek(&sf, out, 1));
    gw_sf_pop(&sf, 1);
    TEST_ASSERT_EQUAL_size_t(1, gw_sf_peek(&sf, out, 8));
    TEST_ASSERT_EQUAL_UINT16(2, out[0].metric);
    gw_sf_pop(&sf, 1);
    TEST_ASSERT_EQUAL_size_t(1, gw_sf_peek(&sf, out, 8));
    TEST_ASSERT_EQUAL_UINT8(GW_SF_NODE, out[0].dev);
    gw_sf_pop(&sf, 1);
    TEST_ASSERT_EQUAL_size_t(1, gw_sf_peek(&sf, out, 8));
    TEST_ASSERT_TRUE(out[0].flags & GW_SF_NULL);
    gw_sf_pop(&sf, 5);   /* more than there is: empty, no underflow */
    TEST_ASSERT_EQUAL_size_t(0, gw_sf_count(&sf));
    TEST_ASSERT_EQUAL_UINT32(4, sf.stored);
    TEST_ASSERT_EQUAL_UINT32(0, sf.dropped);
}

/* Full ring: the oldest samples are overwritten and counted; the newest are kept, in order. */
static void test_overflow_keeps_the_newest(void)
{
    for (int i = 0; i < 20; i++) {
        gw_sf_begin(&sf);
        gw_sf_push(&sf, 3, 7, (double)i, false, (uint64_t)i);
    }
    TEST_ASSERT_EQUAL_size_t(8, gw_sf_count(&sf));
    TEST_ASSERT_EQUAL_UINT32(12, sf.dropped);
    gw_sf_rec_t out[8];
    for (int i = 12; i < 20; i++) {
        TEST_ASSERT_EQUAL_size_t(1, gw_sf_peek(&sf, out, 8));
        TEST_ASSERT_EQUAL_DOUBLE((double)i, out[0].v.num);
        gw_sf_pop(&sf, 1);
    }
    TEST_ASSERT_EQUAL_size_t(0, gw_sf_count(&sf));
}

/* A batch whose first samples were overwritten still comes out (the part that is left). */
static void test_partly_overwritten_batch(void)
{
    gw_sf_begin(&sf);
    for (int i = 0; i < 6; i++) {
        gw_sf_push(&sf, 1, (uint16_t)i, 0, false, 1);
    }
    gw_sf_begin(&sf);
    for (int i = 0; i < 4; i++) {
        gw_sf_push(&sf, 1, (uint16_t)(100 + i), 0, false, 2);
    }
    gw_sf_rec_t out[8];
    TEST_ASSERT_EQUAL_size_t(4, gw_sf_peek(&sf, out, 8));   /* samples 2..5 of the first */
    TEST_ASSERT_EQUAL_UINT16(2, out[0].metric);
    gw_sf_pop(&sf, 4);
    TEST_ASSERT_EQUAL_size_t(4, gw_sf_peek(&sf, out, 8));
    TEST_ASSERT_EQUAL_UINT16(100, out[0].metric);
}

/* No storage (allocation failed): every sample is dropped and counted. */
static void test_without_storage(void)
{
    gw_sf_init(&sf, NULL, 100);
    gw_sf_begin(&sf);
    gw_sf_push(&sf, 0, 0, 1, false, 1);
    TEST_ASSERT_EQUAL_size_t(0, gw_sf_count(&sf));
    TEST_ASSERT_EQUAL_UINT32(1, sf.dropped);
    gw_sf_rec_t out[1];
    TEST_ASSERT_EQUAL_size_t(0, gw_sf_peek(&sf, out, 1));
}

/* MB-6 values: 64-bit integers exact, strings in continuation records and whole in a peek. */
static void test_64bit_and_strings(void)
{
    gw_sf_begin(&sf);
    gw_sf_push_u64(&sf, 2, 5, 0xFFFFFFFFFFFFFFFFull, false, 10);
    gw_sf_push_str(&sf, 2, 6, "ABCDEFGHIJ", false, 11);   /* 10 bytes: head + 2 records */
    TEST_ASSERT_EQUAL_size_t(4, gw_sf_count(&sf));
    TEST_ASSERT_EQUAL_UINT32(2, sf.stored);
    gw_sf_rec_t out[8];
    char s[40];
    /* a window that would cut the string ends before it */
    TEST_ASSERT_EQUAL_size_t(1, gw_sf_peek(&sf, out, 3));
    TEST_ASSERT_EQUAL_UINT64(0xFFFFFFFFFFFFFFFFull, out[0].v.u64);
    TEST_ASSERT_EQUAL_size_t(4, gw_sf_peek(&sf, out, 8));
    TEST_ASSERT_EQUAL_size_t(1, gw_sf_sample(&out[0], 4, s, sizeof(s)));
    TEST_ASSERT_EQUAL_size_t(3, gw_sf_sample(&out[1], 3, s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJ", s);
    /* truncated output buffer, null and empty strings */
    gw_sf_sample(&out[1], 3, s, 4);
    TEST_ASSERT_EQUAL_STRING("ABC", s);
    gw_sf_pop(&sf, 4);
    gw_sf_begin(&sf);
    gw_sf_push_str(&sf, 2, 6, NULL, true, 12);
    gw_sf_push_str(&sf, 2, 6, "", false, 13);
    TEST_ASSERT_EQUAL_size_t(2, gw_sf_peek(&sf, out, 8));
    TEST_ASSERT_EQUAL(GW_SF_STR | GW_SF_NULL, out[0].flags);
    TEST_ASSERT_EQUAL_size_t(1, gw_sf_sample(&out[1], 1, s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("", s);
}

/* A string whose head is overwritten leaves its continuations: they are skipped, not sent. */
static void test_overwritten_string_head(void)
{
    gw_sf_begin(&sf);
    gw_sf_push_str(&sf, 1, 1, "0123456789ABCDEF", false, 1);   /* 3 records */
    for (int i = 0; i < 6; i++) {
        gw_sf_begin(&sf);
        gw_sf_push(&sf, 1, 2, i, false, 2);
    }
    gw_sf_push(&sf, 1, 2, 99, false, 3);   /* 10th record: the head goes */
    TEST_ASSERT_EQUAL_UINT32(1, sf.dropped);
    gw_sf_rec_t out[8];
    TEST_ASSERT_EQUAL_size_t(1, gw_sf_peek(&sf, out, 8));
    TEST_ASSERT_EQUAL_DOUBLE(0, out[0].v.num);
    TEST_ASSERT_EQUAL_size_t(7, gw_sf_count(&sf));
}

/* ---- spill to a (simulated) card (#124: the ring of self-checking blocks) ---- */

#define XB_MAX 8                 /* blocks of an extent, at most, in these tests */
#define XE_MAX 24                /* extent files r0000 .. */
#define NLEG   32                /* step 1 segment files 0 .. NLEG - 1 */
#define LEGB   8192
#define NVAL   14000             /* sample values of a scenario */
#define RAMCAP 512
#define BLK    GW_SFSPILL_BLOCK

typedef struct {
    int32_t size;   /* -1: absent */
    uint32_t cap;
    uint8_t *data;
} mfile_t;

static uint8_t ext_mem[XE_MAX][XB_MAX * BLK];
static uint8_t leg_mem[NLEG][LEGB];
static uint8_t sb_mem[2][GW_SFSPILL_SB_BYTES];

/* The faults of the card at one change (#124 "Tests"). */
enum {
    F_AFTER,    /* the power goes off right after the change */
    F_TORN,     /* the change is cut: a write reaches the card in part (any prefix) */
    F_DROP,     /* the card loses its last writes although it acknowledged them */
    F_DAMAGE,   /* the cut damages one of the last writes (paired pages) */
    F_COUNT
};
static const char *const fault_names[F_COUNT] = {"after", "torn", "drop", "damage"};

typedef struct {
    mfile_t *file;
    uint32_t off, len;
    bool super;
    uint8_t old[BLK];
} wlog_t;

typedef struct {
    mfile_t ext[XE_MAX], leg[NLEG], sb[2];
    bool fail;              /* a broken card: every operation fails */
    bool off;               /* the power is off */
    int changes;            /* operations that changed the card */
    int cut_at, mode;       /* the fault, at change cut_at (-1: none) */
    unsigned seed;
    bool cut_super;         /* the fault touched a superblock */
    int faulty_blocks;      /* acknowledged block writes the fault took back or damaged */
    int64_t space;          /* free bytes, -1: no limit */
    uint32_t op_ms;         /* a slow card: each change takes this long (#136) */
    uint32_t unit;
    int allocs, writes, reads, truncs, removes, lists, sb_writes, block_writes;
    uint32_t max_freed;
    wlog_t log[3];          /* the last writes, newest first */
    int nlog;
    bool durable[NVAL];     /* samples in a block whose write returned true */
    bool overwritten[NVAL]; /* samples of a block that a later block replaced (ring full) */
} card_t;
static card_t card;

static gw_sfspill_io_t io;
static gw_sfspill_t sp;
static uint64_t spmem[(16384 + 7) / 8];
static gw_sf_rec_t ram[RAMCAP];
static uint32_t now_ms;
static uint32_t nonce_n;
static int produced;

static uint32_t next_nonce(void)
{
    return ++nonce_n * 0x9E3779B9u;
}

static unsigned rnd(void)
{
    card.seed = card.seed * 1103515245u + 12345u;
    return (card.seed >> 16) & 0x7FFFu;
}

static mfile_t *file_of(uint32_t f)
{
    uint32_t kind = f & GW_SFSPILL_F_KIND, i = f & ~GW_SFSPILL_F_KIND;
    if (kind == GW_SFSPILL_F_SUPER) {
        return i < 2 ? &card.sb[i] : NULL;
    }
    if (kind == GW_SFSPILL_F_EXTENT) {
        return i < XE_MAX ? &card.ext[i] : NULL;
    }
    return f < NLEG ? &card.leg[f] : NULL;
}

static uint32_t units(int32_t size)
{
    uint32_t u = card.unit != 0 ? card.unit : 1;
    return size <= 0 ? 0 : ((uint32_t)size + u - 1) / u;
}

enum { CH_GO, CH_CUT, CH_OFF };

static int change(void)
{
    if (card.fail || card.off) {
        return CH_OFF;
    }
    card.changes++;
    now_ms += card.op_ms;
    if (card.cut_at >= 0 && card.changes - 1 == card.cut_at) {
        return CH_CUT;
    }
    return CH_GO;
}

/* The power goes off at a cut: after it, every operation fails. */
static void power_off(void)
{
    card.off = true;
}

static int m_alloc(void *ctx, uint32_t f, uint32_t size)
{
    (void)ctx;
    mfile_t *m = file_of(f);
    if (card.fail || card.off || m == NULL || size > m->cap) {
        return -1;
    }
    if ((int32_t)size <= m->size) {
        return 1;   /* whole already: nothing changes */
    }
    int c = change();
    if (c == CH_OFF) {
        return -1;
    }
    card.allocs++;
    int32_t from = m->size < 0 ? 0 : m->size;
    if (c == CH_CUT) {
        /* the FAT is written, the directory entry is not: the file keeps its old size (or is
         * empty); its new clusters are lost */
        m->size = from;
        power_off();
        return -1;
    }
    int64_t need = (int64_t)size - from;
    if (card.space >= 0 && need > card.space) {
        m->size = from + (int32_t)card.space;   /* what fits */
        card.space = 0;
        return 0;
    }
    if (card.space >= 0) {
        card.space -= need;
    }
    m->size = (int32_t)size;   /* the new space keeps what was there before (FAT does not zero) */
    return 1;
}

static void note_samples(const uint8_t *d, bool *set)
{
    gw_sfspill_blk_t h;
    memcpy(&h, d, sizeof(h));
    if (h.magic != GW_SFSPILL_BLK_MAGIC) {
        return;
    }
    for (uint32_t i = 0; i < h.nrec && i < GW_SFSPILL_BLK_RECS; i++) {
        gw_sf_rec_t r;
        memcpy(&r, d + 32 + i * sizeof(r), sizeof(r));
        uint64_t c = r.ts_ms / 10;
        if (c < NVAL) {
            set[c] = true;
        }
    }
}

static bool m_write(void *ctx, uint32_t f, uint32_t off, const void *d, size_t len)
{
    (void)ctx;
    mfile_t *m = file_of(f);
    if (m == NULL || m->size < 0 || off + len > (uint32_t)m->size) {
        return false;   /* never extends a file */
    }
    bool ext = (f & GW_SFSPILL_F_KIND) == GW_SFSPILL_F_EXTENT;
    bool super = (f & GW_SFSPILL_F_KIND) == GW_SFSPILL_F_SUPER;
    if (ext) {
        /* #124: whole aligned blocks, never part of one; a string sample never starts in one
         * block and ends in the next */
        TEST_ASSERT_EQUAL_size_t(BLK, len);
        TEST_ASSERT_EQUAL_UINT32(0, off % BLK);
        gw_sf_rec_t first;
        memcpy(&first, (const uint8_t *)d + 32, sizeof(first));
        TEST_ASSERT_FALSE(first.flags & GW_SF_CONT);
    }
    if (super) {
        TEST_ASSERT_EQUAL_UINT32(0, off);
        TEST_ASSERT_EQUAL_size_t(GW_SFSPILL_SB_BYTES, len);
    }
    int c = change();
    if (c == CH_OFF) {
        return false;
    }
    /* the log of the last writes (what was there before each) */
    memmove(&card.log[1], &card.log[0], sizeof(card.log[0]) * 2);
    card.log[0].file = m;
    card.log[0].off = off;
    card.log[0].len = (uint32_t)len;
    card.log[0].super = super;
    memcpy(card.log[0].old, m->data + off, len);
    card.nlog = card.nlog < 3 ? card.nlog + 1 : 3;
    card.writes++;
    if (c == CH_CUT && card.mode == F_TORN) {
        size_t k = len > 1 ? rnd() % len : 0;   /* any prefix, also inside a sector */
        if (ext) {
            /* the block it replaces (dropped for it already, ring full) is destroyed too: a
             * hole if the replay meets it */
            note_samples(m->data + off, card.overwritten);
            card.faulty_blocks++;
        }
        memcpy(m->data + off, d, k);
        card.cut_super = super;
        power_off();
        return false;
    }
    if (ext) {
        note_samples(m->data + off, card.overwritten);
    }
    memcpy(m->data + off, d, len);
    if (ext) {
        card.block_writes++;
        note_samples(d, card.durable);
    }
    if (super) {
        card.sb_writes++;
    }
    if (c == CH_CUT) {
        if (card.mode == F_DROP) {
            int n = 1 + (int)(rnd() % 2u);
            n = n < card.nlog ? n : card.nlog;
            for (int i = 0; i < n; i++) {   /* newest first */
                wlog_t *w = &card.log[i];
                memcpy(w->file->data + w->off, w->old, w->len);
                card.cut_super |= w->super;
                card.faulty_blocks += !w->super && w->len == BLK;
            }
        } else if (card.mode == F_DAMAGE) {
            wlog_t *w = &card.log[rnd() % (unsigned)card.nlog];
            w->file->data[w->off + rnd() % w->len] ^= 0x10;
            card.cut_super |= w->super;
            card.faulty_blocks += !w->super && w->len == BLK;
        }
        power_off();
    }
    return true;
}

static int m_read(void *ctx, uint32_t f, uint32_t off, void *d, size_t len)
{
    (void)ctx;
    if (card.fail || card.off) {
        return -1;
    }
    mfile_t *m = file_of(f);
    if (m == NULL || m->size < 0 || off + len > (uint32_t)m->size) {
        return 0;
    }
    card.reads++;
    memcpy(d, m->data + off, len);
    return 1;
}

static void freed(int32_t from, int32_t to)
{
    uint32_t n = units(from) - units(to);
    card.max_freed = n > card.max_freed ? n : card.max_freed;
    if (card.space >= 0) {
        card.space += from - to;
    }
}

/* A cut truncate is lost (FAT writes the directory entry with the fsync, at the end). */
static bool m_truncate(void *ctx, uint32_t f, uint32_t len)
{
    (void)ctx;
    mfile_t *m = file_of(f);
    if (m == NULL || m->size < 0 || len >= (uint32_t)m->size) {
        return false;
    }
    int c = change();
    if (c != CH_GO) {
        if (c == CH_CUT) {
            power_off();
        }
        return false;
    }
    card.truncs++;
    freed(m->size, (int32_t)len);
    m->size = (int32_t)len;
    return true;
}

/* A cut unlink still removes the file (FAT removes the directory entry first). */
static bool m_remove(void *ctx, uint32_t f)
{
    (void)ctx;
    mfile_t *m = file_of(f);
    if (m == NULL || m->size < 0) {
        return !card.fail && !card.off;
    }
    int c = change();
    if (c == CH_OFF) {
        return false;
    }
    card.removes++;
    freed(m->size, 0);
    m->size = -1;
    if (c == CH_CUT) {
        power_off();
        return false;
    }
    return true;
}

static int32_t m_size(void *ctx, uint32_t f)
{
    (void)ctx;
    mfile_t *m = file_of(f);
    return card.fail || card.off || m == NULL ? -1 : m->size;
}

static bool m_list(void *ctx, void (*each)(void *arg, uint32_t f), void *arg)
{
    (void)ctx;
    if (card.fail || card.off) {
        return false;
    }
    card.lists++;
    for (uint32_t i = 0; i < 2; i++) {
        if (card.sb[i].size >= 0) {
            each(arg, GW_SFSPILL_F_SUPER | i);
        }
    }
    for (uint32_t i = 0; i < XE_MAX; i++) {
        if (card.ext[i].size >= 0) {
            each(arg, GW_SFSPILL_F_EXTENT | i);
        }
    }
    for (uint32_t i = 0; i < NLEG; i++) {
        if (card.leg[i].size >= 0) {
            each(arg, i);
        }
    }
    return true;
}

/* A new, empty card (FAT cluster @p unit bytes). */
static void card_new(uint32_t unit)
{
    memset(&card, 0, sizeof(card));
    memset(ext_mem, 0, sizeof(ext_mem));
    memset(leg_mem, 0, sizeof(leg_mem));
    memset(sb_mem, 0, sizeof(sb_mem));
    for (int i = 0; i < XE_MAX; i++) {
        card.ext[i] = (mfile_t){-1, sizeof(ext_mem[i]), ext_mem[i]};
    }
    for (int i = 0; i < NLEG; i++) {
        card.leg[i] = (mfile_t){-1, LEGB, leg_mem[i]};
    }
    for (int i = 0; i < 2; i++) {
        card.sb[i] = (mfile_t){-1, GW_SFSPILL_SB_BYTES, sb_mem[i]};
    }
    card.cut_at = -1;
    card.space = -1;
    card.unit = unit;
    io = (gw_sfspill_io_t){&card, unit, m_alloc, m_write, m_read, m_truncate, m_remove, m_size,
                           m_list};
    produced = 0;
}

/* A (re)boot: an empty RAM ring and a spill of E extents of M blocks on the card. */
static void boot(uint32_t ext_blocks, uint32_t extents, uint32_t hash)
{
    gw_sf_init(&sf, ram, RAMCAP);
    gw_sfspill_init(&sp, &io, ext_blocks, extents, hash, spmem);
    sp.random = next_nonce;
    gw_sf_attach_spill(&sf, &sp);
}

/* One gw_sf_step() (one pass of the Sparkplug loop): #124 bounds its work. */
static void pass_step(void)
{
    if (card.off) {
        return;
    }
    int changes = card.changes, reads = card.reads;
    now_ms += 10;
    gw_sf_step(&sf, now_ms);
    TEST_ASSERT_TRUE(card.changes - changes <= 1);   /* one bounded change of the card */
    TEST_ASSERT_TRUE(card.reads - reads <= 2 * GW_SFSPILL_SCAN_READS);
}

/* Steps until the boot recovery is done (and a new superblock written); returns them. */
static int until_ready(void)
{
    int steps = 0;
    while (!card.off && sp.ok && (sp.phase != GW_SFSPILL_READY || sp.sb_due)) {
        pass_step();
        TEST_ASSERT_LESS_THAN_INT(3000, ++steps);
    }
    return steps;
}

/* All the steps that are due (deletes, superblocks). */
static int steps_all(void)
{
    int steps = 0;
    while (!card.off && gw_sf_busy(&sf)) {
        pass_step();
        TEST_ASSERT_LESS_THAN_INT(5000, ++steps);
    }
    return steps;
}

/* @p n samples while offline: a batch every 10, one pass of the loop (a card step) after it.
 * Sample c: time c * 10 + 5, tag c % 5, its value c; every 17th a string made from c. */
static void push_n(int n)
{
    for (int i = 0; i < n && !card.off; i++) {
        int c = produced++;
        TEST_ASSERT_LESS_THAN_INT(NVAL, c);
        if (c % 10 == 0) {
            gw_sf_begin(&sf);
        }
        uint64_t ts = (uint64_t)c * 10u + 5u;
        if (c % 17 == 0) {
            char s[24];
            snprintf(s, sizeof(s), "s%05d-abcdefgh", c);
            gw_sf_push_str(&sf, 0, (uint16_t)(c % 5), s, false, ts);
        } else {
            gw_sf_push(&sf, 0, (uint16_t)(c % 5), (double)c, false, ts);
        }
        if (c % 10 == 9) {
            pass_step();
        }
    }
}

/* What a replay saw. */
typedef struct {
    int last;               /* the last sample of this boot: the replay goes up */
    int got;                /* samples */
    int dups;               /* samples consumed before the reboot, sent again */
    bool seen[NVAL];
} replay_t;
static bool consumed[NVAL];   /* samples published (and popped) before the reboot */
static replay_t rp;

static void replay_reset(void)
{
    memset(&rp, 0, sizeof(rp));
    rp.last = -1;
}

/* Check one sample (no garbage: every field from its counter); returns its records. */
static size_t check_sample(const gw_sf_rec_t *r, size_t n, const char *msg)
{
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(5, r[0].ts_ms % 10, msg);
    uint64_t c = r[0].ts_ms / 10;
    TEST_ASSERT_TRUE_MESSAGE(c < (uint64_t)produced, msg);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, r[0].dev, msg);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(c % 5, r[0].metric, msg);
    size_t used = 1;
    if (c % 17 == 0) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(GW_SF_STR, r[0].flags, msg);
        char s[40], want[24];
        used = gw_sf_sample(r, n, s, sizeof(s));
        snprintf(want, sizeof(want), "s%05d-abcdefgh", (int)c);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(want, s, msg);
        for (size_t i = 1; i < used; i++) {
            TEST_ASSERT_EQUAL_UINT64_MESSAGE(r[0].ts_ms, r[i].ts_ms, msg);
        }
    } else {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, r[0].flags, msg);
        TEST_ASSERT_EQUAL_DOUBLE_MESSAGE((double)c, r[0].v.num, msg);
    }
    TEST_ASSERT_TRUE_MESSAGE((int)c > rp.last, msg);   /* in order, once in this boot */
    rp.last = (int)c;
    rp.got++;
    rp.dups += consumed[c];
    rp.seen[c] = true;
    return used;
}

/* One pass of the loop while online: up to 4 historical messages, then a card step. */
static int pass_online(const char *msg)
{
    gw_sf_rec_t out[128];
    int n = 0;
    for (int m = 0; m < 4 && !card.off; m++) {
        size_t k = gw_sf_peek(&sf, out, 128);
        if (k == 0) {
            break;
        }
        for (size_t i = 0; i < k;) {
            i += check_sample(&out[i], k - i, msg);
        }
        gw_sf_pop(&sf, k);
        n += (int)k;
    }
    pass_step();
    return n;
}

/* Replay until @p want samples (or everything, want < 0) went out; returns the samples. */
static int replay(int want, const char *msg)
{
    int before = rp.got;
    for (int passes = 0; !card.off && (want < 0 || rp.got - before < want); passes++) {
        if (gw_sf_count(&sf) == 0 && !gw_sf_busy(&sf)) {
            break;
        }
        pass_online(msg);
        TEST_ASSERT_LESS_THAN_INT_MESSAGE(100000, passes, msg);
    }
    return rp.got - before;
}

/* The samples of this boot's replay become "consumed" (the next boot's duplicates). */
static void remember_consumed(void)
{
    for (int c = 0; c < NVAL; c++) {
        consumed[c] |= rp.seen[c];
    }
}

static void test_crc_and_plan(void)
{
    TEST_ASSERT_EQUAL_HEX32(0xE3069283u, gw_sfspill_crc32c(0, "123456789", 9));
    TEST_ASSERT_EQUAL_HEX32(0xE3069283u,
                            gw_sfspill_crc32c(gw_sfspill_crc32c(0, "1234", 4), "56789", 5));
    TEST_ASSERT_EQUAL_UINT32(0x811C9DC5u, gw_sfspill_fnv(GW_SFSPILL_FNV_INIT, "", 0));
    TEST_ASSERT_EQUAL_UINT32(0xE40C292Cu, gw_sfspill_fnv(GW_SFSPILL_FNV_INIT, "a", 1));
    TEST_ASSERT_EQUAL_size_t(32, sizeof(gw_sfspill_blk_t));
    TEST_ASSERT_EQUAL_size_t(72, sizeof(gw_sfspill_sb_t));
    TEST_ASSERT_TRUE(gw_sfspill_mem_size() <= sizeof(spmem));
    uint32_t m, e;
    /* the default 128 MiB: with 4 KiB clusters 256 extents of 512 KiB (#124) */
    TEST_ASSERT_EQUAL_UINT32(131072, gw_sfspill_plan(GW_SFSPILL_DEFAULT_KB, 4096, &m, &e));
    TEST_ASSERT_EQUAL_UINT32(128, m);
    TEST_ASSERT_EQUAL_UINT32(256, e);
    TEST_ASSERT_EQUAL_UINT32(131072, gw_sfspill_plan(131072, 32768, &m, &e));   /* 4 MiB */
    TEST_ASSERT_EQUAL_UINT32(1024, m);
    TEST_ASSERT_EQUAL_UINT32(32, e);
    /* the maximum: 4 GiB with 32 KiB clusters; 512 MiB at most with 4 KiB ones */
    TEST_ASSERT_EQUAL_UINT32(4194304, gw_sfspill_plan(GW_SFSPILL_MAX_KB, 32768, &m, &e));
    TEST_ASSERT_EQUAL_UINT32(1024, e);
    TEST_ASSERT_EQUAL_UINT32(524288, gw_sfspill_plan(GW_SFSPILL_MAX_KB, 4096, &m, &e));
    TEST_ASSERT_EQUAL_UINT32(GW_SFSPILL_MAX_EXTENTS, e);
    /* the minimum: one smaller extent */
    TEST_ASSERT_EQUAL_UINT32(64, gw_sfspill_plan(GW_SFSPILL_MIN_KB, 32768, &m, &e));
    TEST_ASSERT_EQUAL_UINT32(16, m);
    TEST_ASSERT_EQUAL_UINT32(1, e);
    TEST_ASSERT_EQUAL_UINT32(64, gw_sfspill_plan(8, 32768, &m, &e));   /* clamped */
    /* whole extents: 100 KB of 64 KiB extents (512 B clusters) is one */
    TEST_ASSERT_EQUAL_UINT32(64, gw_sfspill_plan(100, 512, &m, &e));
    TEST_ASSERT_EQUAL_UINT32(16, m);
    /* unknown cluster: 4 MiB; the QEMU stand-in (32 B units): one block per extent */
    TEST_ASSERT_EQUAL_UINT32(16384, gw_sfspill_plan(16384, 0, &m, &e));
    TEST_ASSERT_EQUAL_UINT32(1024, m);
    TEST_ASSERT_EQUAL_UINT32(4, e);
    TEST_ASSERT_EQUAL_UINT32(128, gw_sfspill_plan(128, 32, &m, &e));
    TEST_ASSERT_EQUAL_UINT32(1, m);
    TEST_ASSERT_EQUAL_UINT32(32, e);
}

/* Offline the ring spills its oldest records, a block at a time, from three quarters full;
 * they come back first, then the ring: nothing lost, in order, strings whole. The steady state
 * only writes whole blocks (no allocation once the extents exist, no truncate, no delete). */
static void test_spill_and_replay(void)
{
    card_new(4096);
    boot(2, 8, 0x1234);
    TEST_ASSERT_EQUAL_INT(0, card.changes);    /* gw_sfspill_init() does no I/O */
    until_ready();
    TEST_ASSERT_EQUAL_INT(1, card.sb_writes);  /* the new ring's superblock, before any block */
    push_n(2000);
    TEST_ASSERT_EQUAL_UINT32(0, sf.dropped);
    TEST_ASSERT_TRUE(sp.blocks >= 9);
    TEST_ASSERT_EQUAL_UINT64(sf.spilled, sp.written);
    TEST_ASSERT_EQUAL_size_t(2000 + 2000 / 17 * 2 + 2, gw_sf_count(&sf));   /* + continuations */
    TEST_ASSERT_TRUE(sf.count < RAMCAP);
    int allocs = card.allocs;
    TEST_ASSERT_EQUAL_INT((int)(sp.blocks + 1) / 2 + 2, allocs);   /* extents + 2 superblocks */
    replay_reset();
    TEST_ASSERT_EQUAL_INT(2000, replay(-1, "replay"));
    for (int c = 0; c < 2000; c++) {
        TEST_ASSERT_TRUE(rp.seen[c]);
    }
    TEST_ASSERT_EQUAL_size_t(0, gw_sf_count(&sf));
    steps_all();
    TEST_ASSERT_EQUAL_INT(0, card.truncs + card.removes);
    TEST_ASSERT_EQUAL_INT(allocs, card.allocs);
    TEST_ASSERT_EQUAL_UINT32(1, sp.checkpoints);      /* everything replayed */
    TEST_ASSERT_TRUE(sp.ck_max <= GW_SFSPILL_CKPT_RECS);
    TEST_ASSERT_EQUAL_UINT32(0, sp.holes);
    TEST_ASSERT_FALSE(gw_sf_busy(&sf));
}

/* Card full: the ring overwrites its oldest blocks (the newest data is kept) and counts what it
 * drops; the replay is exactly the newest samples. After the first lap no FAT work at all. */
static void test_spill_wrap_keeps_newest(void)
{
    card_new(4096);
    boot(1, 4, 0x2222);   /* 4 blocks */
    until_ready();
    push_n(3000);
    TEST_ASSERT_EQUAL_INT(6, card.allocs);   /* 4 extents and 2 superblock files, once */
    TEST_ASSERT_TRUE(sp.blocks > 4);
    TEST_ASSERT_TRUE(sp.dropped > 0);
    TEST_ASSERT_EQUAL_UINT32(0, sf.dropped);
    replay_reset();
    int n = replay(-1, "wrap");
    int first = produced - n;
    for (int c = 0; c < produced; c++) {
        TEST_ASSERT_EQUAL(c >= first, rp.seen[c]);   /* the newest, without a gap */
    }
    /* every record that is not replayed was counted as dropped */
    uint32_t recs_before = 0;
    for (int c = 0; c < first; c++) {
        recs_before += c % 17 == 0 ? 3u : 1u;
    }
    TEST_ASSERT_EQUAL_UINT32(recs_before, sp.dropped);
    TEST_ASSERT_EQUAL_INT(0, card.truncs + card.removes);
    TEST_ASSERT_EQUAL_INT(6, card.allocs);
}

/* A reboot keeps what is on the card: the boot recovery finds it in bounded steps (from the
 * superblock's head hint), the replay sends it once; after a full replay the checkpoint stops
 * a second replay; a reboot during a replay sends at most 64 KiB again (#124). */
static void test_spill_reboot(void)
{
    card_new(4096);
    boot(4, 16, 0x3333);   /* 64 blocks */
    until_ready();
    push_n(8000);
    uint64_t on_card = sp.ring_count;
    TEST_ASSERT_TRUE(sp.sb_writes >= 2);   /* the head hint, every 32 blocks */
    /* reboot: the RAM ring is gone, the card is not */
    boot(4, 16, 0x3333);
    int reads = card.reads;
    int steps = until_ready();
    TEST_ASSERT_TRUE(steps < 20);
    TEST_ASSERT_TRUE(card.reads - reads <= 2 + GW_SFSPILL_HINT_BLOCKS + GW_SFSPILL_HOLES_MAX + 1);
    TEST_ASSERT_EQUAL_UINT64(on_card, sp.ring_count);   /* exact: from the superblock + scan */
    TEST_ASSERT_EQUAL_UINT32(sp.kept_at_boot, (uint32_t)on_card);
    memset(consumed, 0, sizeof(consumed));
    replay_reset();
    replay(5000, "reboot 1");   /* a replay of about 5000 samples, then the power goes */
    TEST_ASSERT_TRUE(sp.checkpoints >= 1);
    TEST_ASSERT_TRUE(sp.ck_max <= GW_SFSPILL_CKPT_RECS);
    remember_consumed();
    boot(4, 16, 0x3333);
    until_ready();
    replay_reset();
    replay(-1, "reboot 2");
    TEST_ASSERT_TRUE(rp.dups > 0);
    TEST_ASSERT_TRUE(rp.dups <= (int)GW_SFSPILL_CKPT_RECS);   /* at most 64 KiB again */
    for (int c = 0; c < produced; c++) {
        if (card.durable[c]) {
            TEST_ASSERT_TRUE(consumed[c] || rp.seen[c]);   /* nothing lost */
        }
    }
    steps_all();   /* the final checkpoint */
    remember_consumed();
    boot(4, 16, 0x3333);
    until_ready();
    TEST_ASSERT_EQUAL_size_t(0, gw_sf_count(&sf));   /* nothing again */
    replay_reset();
    TEST_ASSERT_EQUAL_INT(0, replay(-1, "reboot 3"));
}

/* #124: a change of the devices or tags resets the ring with a new nonce: nothing of the old
 * ring is replayed, and no file is deleted for it; changing back does not revive it. */
static void test_spill_config_change(void)
{
    card_new(4096);
    boot(2, 8, 0xA);
    until_ready();
    push_n(1500);
    uint32_t nonce_a = sp.nonce;
    uint64_t old = sp.ring_count;
    int changes = card.changes;
    boot(2, 8, 0xB);
    until_ready();
    TEST_ASSERT_EQUAL_INT(changes + 1, card.changes);   /* only the new superblock */
    TEST_ASSERT_NOT_EQUAL(nonce_a, sp.nonce);
    TEST_ASSERT_TRUE(old > 1000);
    TEST_ASSERT_EQUAL_UINT64(0, gw_sf_count(&sf));
    TEST_ASSERT_EQUAL_INT(0, card.removes + card.truncs);
    produced = 0;
    memset(consumed, 0, sizeof(consumed));
    push_n(900);
    replay_reset();
    TEST_ASSERT_EQUAL_INT(900, replay(-1, "config B"));   /* only the new ring's samples */
    /* back to the first configuration: the old blocks still have a nonce of no ring */
    boot(2, 8, 0xA);
    until_ready();
    TEST_ASSERT_EQUAL_UINT64(0, gw_sf_count(&sf));
    replay_reset();
    TEST_ASSERT_EQUAL_INT(0, replay(-1, "config A"));
    /* the new ring writes a few blocks; after a reboot the scan stops at them, although the
     * blocks after them are of this configuration, at their position, and sound: the nonce */
    produced = 0;
    memset(consumed, 0, sizeof(consumed));
    push_n(700);
    TEST_ASSERT_TRUE(sp.blocks >= 1 && sp.blocks < 6);
    boot(2, 8, 0xA);
    until_ready();
    replay_reset();
    int n = replay(-1, "config A again");
    TEST_ASSERT_TRUE(n > 100 && n < 700);
}

/* A smaller ring (max_kb) deletes the extents beyond it, one per step; a larger one adds
 * extents when it needs them. */
static void test_spill_geometry_change(void)
{
    card_new(4096);
    boot(2, 8, 0xC);
    until_ready();
    push_n(3000);
    TEST_ASSERT_EQUAL_INT32(2 * BLK, card.ext[7].size);
    boot(2, 3, 0xC);
    until_ready();
    TEST_ASSERT_EQUAL_UINT32(5, gw_sfspill_gc_pending(&sp));
    int removes = card.removes;
    TEST_ASSERT_EQUAL_INT(5, steps_all());   /* one unlink each: an extent is one FAT step */
    TEST_ASSERT_EQUAL_INT(removes + 5, card.removes);
    for (int i = 0; i < XE_MAX; i++) {
        TEST_ASSERT_EQUAL(i < 3, card.ext[i].size >= 0);
    }
    TEST_ASSERT_TRUE(card.max_freed <= GW_SFSPILL_STEP_UNITS);
    TEST_ASSERT_EQUAL_UINT64(0, gw_sf_count(&sf));   /* a new geometry: a new ring */
    boot(2, 6, 0xC);
    until_ready();
    produced = 0;
    memset(consumed, 0, sizeof(consumed));
    push_n(1900);
    TEST_ASSERT_EQUAL_INT32(2 * BLK, card.ext[5].size);
    replay_reset();
    TEST_ASSERT_EQUAL_INT(1900, replay(-1, "geometry"));
}

/* The card fills up while the ring is allocated the first time: the ring stops at the extents
 * it has (the same nonce, a new superblock before it wraps) and keeps the newest data; the
 * next boot keeps that size. */
static void test_spill_card_full(void)
{
    card_new(4096);
    card.space = 3 * 2 * BLK + 2 * GW_SFSPILL_SB_BYTES + 100;
    boot(2, 8, 0xD);
    until_ready();
    push_n(3000);
    TEST_ASSERT_TRUE(sp.ok);
    TEST_ASSERT_EQUAL_UINT32(6, sp.nblocks);
    TEST_ASSERT_TRUE(sp.dropped > 0);
    steps_all();   /* the extent that did not fit is deleted */
    TEST_ASSERT_EQUAL_INT32(-1, card.ext[3].size);
    boot(2, 8, 0xD);
    until_ready();
    TEST_ASSERT_EQUAL_UINT32(6, sp.nblocks);
    replay_reset();
    int n = replay(-1, "full");
    TEST_ASSERT_TRUE(n > 6 * 100);
    int first = 0;
    while (!rp.seen[first]) {
        first++;
    }
    for (int c = first; c <= rp.last; c++) {
        TEST_ASSERT_TRUE(rp.seen[c]);   /* the newest blocks, without a gap */
    }
    /* a card with no room at all: the spill stops, the RAM ring goes on */
    card_new(4096);
    card.space = 2 * GW_SFSPILL_SB_BYTES;
    boot(2, 8, 0xD);
    until_ready();
    push_n(1000);
    TEST_ASSERT_FALSE(sp.ok);
    TEST_ASSERT_EQUAL_STRING("card full", sp.why);
    TEST_ASSERT_TRUE(sf.dropped > 0);
}

/* Both superblock copies destroyed: the nonce comes back from the block headers (a vote over
 * the first block of each extent); the data is replayed (the position is lost: duplicates are
 * possible, garbage is not). */
static void test_spill_superblocks_lost(void)
{
    card_new(4096);
    boot(2, 8, 0xE);
    until_ready();
    push_n(2000);
    uint32_t nonce = sp.nonce;
    memset(sb_mem, 0x5A, sizeof(sb_mem));
    boot(2, 8, 0xE);
    until_ready();
    TEST_ASSERT_TRUE(sp.ok);
    TEST_ASSERT_EQUAL_HEX32(nonce, sp.nonce);
    memset(consumed, 0, sizeof(consumed));
    replay_reset();
    int n = replay(-1, "probe");
    for (int c = 0; c < produced; c++) {
        if (card.durable[c]) {
            TEST_ASSERT_TRUE(rp.seen[c]);
        }
    }
    TEST_ASSERT_TRUE(n > 1500);
    TEST_ASSERT_EQUAL_UINT32(0, sp.holes);
    /* nothing valid at all: a new ring */
    memset(sb_mem, 0, sizeof(sb_mem));
    memset(ext_mem, 0xA5, sizeof(ext_mem));
    boot(2, 8, 0xE);
    until_ready();
    TEST_ASSERT_TRUE(sp.ok);
    TEST_ASSERT_NOT_EQUAL(nonce, sp.nonce);
    TEST_ASSERT_EQUAL_size_t(0, gw_sf_count(&sf));
}

/* A damaged block in the middle is a hole: counted, its records lost, never replayed; the
 * blocks after it come back. */
static void test_spill_hole(void)
{
    card_new(4096);
    boot(2, 8, 0xF);
    until_ready();
    push_n(2000);
    TEST_ASSERT_TRUE(sp.blocks > 6);
    ext_mem[1][BLK + 100] ^= 1;   /* block 3 (extent 1, its second block) */
    boot(2, 8, 0xF);
    until_ready();
    memset(consumed, 0, sizeof(consumed));
    replay_reset();
    int n = replay(-1, "hole");
    TEST_ASSERT_EQUAL_UINT32(1, sp.holes);
    int lost = 0;
    for (int c = 0; c < produced; c++) {
        lost += card.durable[c] && !rp.seen[c];
    }
    TEST_ASSERT_TRUE(lost > 100 && lost <= (int)GW_SFSPILL_BLK_RECS);
    TEST_ASSERT_TRUE(n + lost >= 2000 - RAMCAP);
}

/* Write a step 1 segment (format version 1): header, @p n records of numeric samples from
 * @p from (multiples of 17 are strings in these tests: skipped), and @p partial bytes of one
 * more (a power cut). Returns the next sample. */
static int old_segment(uint32_t seg, uint32_t hash, int from, int n, int partial)
{
    gw_sfspill_hdr_t h = {GW_SFSPILL_MAGIC, GW_SFSPILL_VERSION, 24, hash, seg};
    memcpy(leg_mem[seg], &h, sizeof(h));
    int c = from;
    for (int i = 0; i < n + (partial > 0); i++, c++) {
        c += c % 17 == 0;
        gw_sf_rec_t r = {.ts_ms = (uint64_t)c * 10u + 5u, .metric = (uint16_t)(c % 5), .batch = 1};
        r.v.num = c;
        memcpy(leg_mem[seg] + sizeof(h) + (size_t)i * sizeof(r), &r, sizeof(r));
    }
    card.leg[seg].size = (int32_t)(sizeof(h) + (size_t)n * sizeof(gw_sf_rec_t)) + partial;
    return c - (partial > 0);
}

/* Migration from step 1 (#122 format): the segments of this configuration are replayed once,
 * before the ring, then deleted in bounded steps (dead mark, truncates of 128 clusters, the
 * unlink); the others are deleted unreplayed. A reboot during their replay repeats at most
 * 64 KiB (the superblock keeps the position). */
static void test_spill_migration(void)
{
    card_new(8);   /* a delete step: 128 units of 8 B, 1024 B */
    old_segment(3, 0x31, 1, 16, 0);
    old_segment(4, 0x31, 18, 16, 5);     /* the newest: and 5 bytes of a cut record */
    old_segment(5, 0x99, 500, 300, 0);   /* another configuration: 7216 B */
    old_segment(6, 0x31, 900, 3, 0);
    memcpy(leg_mem[6], "DEAD", 4);       /* a delete that a power cut stopped */
    card.leg[7].size = 3;                /* a torn file */
    produced = 40;
    boot(2, 8, 0x31);
    until_ready();
    TEST_ASSERT_EQUAL_UINT32(32, sp.legacy_found);
    TEST_ASSERT_EQUAL_UINT32(3, gw_sfspill_gc_pending(&sp));
    TEST_ASSERT_EQUAL_size_t(32, gw_sf_count(&sf));
    /* new samples go to the ring behind them */
    push_n(1000);
    memset(consumed, 0, sizeof(consumed));
    replay_reset();
    TEST_ASSERT_EQUAL_INT(1032, replay(-1, "migration"));
    for (int c = 1; c < 34; c++) {
        TEST_ASSERT_EQUAL(c != 17, rp.seen[c]);
    }
    steps_all();
    for (int i = 0; i < NLEG; i++) {
        TEST_ASSERT_EQUAL_INT32(-1, card.leg[i].size);   /* all deleted */
    }
    TEST_ASSERT_TRUE(card.max_freed <= GW_SFSPILL_STEP_UNITS);
    /* segment 5: mark, 7 truncates, unlink; 6 (dead already) and 7: unlink; 3 and 4
     * (replayed): mark, unlink */
    TEST_ASSERT_EQUAL_UINT32(9 + 1 + 1 + 2 + 2, sp.gc_steps);
    TEST_ASSERT_EQUAL_UINT32(5, sp.gc_files);
    /* a reboot during their replay: at most 64 KiB again, nothing lost */
    card_new(8);
    int next = old_segment(3, 0x31, 1, 150, 0);
    int end = old_segment(4, 0x31, next, 300, 0);
    produced = end;
    boot(2, 8, 0x31);
    until_ready();
    memset(consumed, 0, sizeof(consumed));
    replay_reset();
    replay(200, "migration 1");   /* segment 3 and a part of 4 */
    remember_consumed();
    steps_all();
    boot(2, 8, 0x31);
    until_ready();
    replay_reset();
    replay(-1, "migration 2");
    TEST_ASSERT_TRUE(rp.dups > 0);
    TEST_ASSERT_TRUE(rp.dups <= (int)GW_SFSPILL_CKPT_RECS);
    for (int c = 1; c < end; c++) {
        TEST_ASSERT_EQUAL(c % 17 != 0, rp.seen[c] || consumed[c]);
    }
}

/* #134: after an I/O failure the samples on the card are not counted as waiting any more
 * (stranded), nothing asks for the loop's time, and the RAM ring goes on as without a card. */
static void test_spill_io_error(void)
{
    card_new(4096);
    boot(2, 8, 0x40);
    until_ready();
    push_n(1500);
    uint64_t on_card = sp.ring_count;
    TEST_ASSERT_TRUE(on_card > 0);
    card.fail = true;
    push_n(1000);
    TEST_ASSERT_FALSE(sp.ok);
    TEST_ASSERT_EQUAL_UINT64(on_card, sp.stranded);
    TEST_ASSERT_EQUAL_UINT64(0, gw_sfspill_waiting(&sp));
    TEST_ASSERT_EQUAL_size_t(sf.count, gw_sf_count(&sf));   /* the RAM ring only */
    TEST_ASSERT_FALSE(gw_sf_busy(&sf));
    TEST_ASSERT_FALSE(gw_sf_step(&sf, now_ms));
    TEST_ASSERT_TRUE(sf.dropped > 0);   /* the ring overwrites as without a card */
    replay_reset();
    int n = replay(-1, "after the failure");
    TEST_ASSERT_EQUAL_INT(n, rp.got);
    TEST_ASSERT_TRUE(n > 0 && n <= RAMCAP);
    TEST_ASSERT_EQUAL_size_t(0, gw_sf_count(&sf));
}

/* The scenarios of the fault test. 0: offline, a replay across a checkpoint, offline again,
 * the rest. 1: a small ring that wraps (full card: blocks overwritten, head hints), a replay
 * across checkpoints, more. Each stops where the power goes off. */
static const uint32_t fault_geometry[2][2] = {{4, 8}, {2, 4}};

static void fault_scenario(int sc)
{
    boot(fault_geometry[sc][0], fault_geometry[sc][1], 0x77);
    until_ready();
    replay_reset();
    if (sc == 0) {
        push_n(3000);
        replay(2800, "before the cut");
        push_n(1500);
    } else {
        push_n(2500);
        replay(1400, "before the cut");
        push_n(2000);
        replay(3000, "before the cut");
        push_n(900);
    }
    replay(-1, "before the cut");
    steps_all();
    remember_consumed();
}

/* #124: a power cut at each change of the card, with each fault: the power goes off after it,
 * the change is torn, the card drops its last acknowledged writes, or damages one. After the
 * reboot: the recovery ends in bounded steps, no garbage is replayed, the order holds, the
 * loss is only what the fault took (acknowledged blocks the card dropped or damaged), and the
 * duplicates stay within 64 KiB (128 KiB when the fault hit the checkpoint itself). */
static void test_spill_faults(void)
{
    for (int sc = 0; sc < 2; sc++) {
        card_new(4096);
        memset(consumed, 0, sizeof(consumed));
        fault_scenario(sc);
        int total = card.changes;
        printf("fault test %d: %d changes of the card, %d blocks, %d superblocks, %lu samples "
               "dropped by a full card; each cut 4 ways\n",
               sc, total, card.block_writes, card.sb_writes, (unsigned long)sp.dropped);
        TEST_ASSERT_TRUE(total > 30);
        TEST_ASSERT_TRUE(sc == 0 ? sp.dropped == 0 : sp.dropped > 0);
        int runs = 0, with_holes = 0, with_dups = 0, max_dups = 0, max_lost = 0;
        for (int mode = 0; mode < F_COUNT; mode++) {
            for (int cut = 0; cut < total; cut++) {
                char msg[64];
                snprintf(msg, sizeof(msg), "scenario %d, cut %s at change %d", sc,
                         fault_names[mode], cut);
                card_new(4096);
                card.cut_at = cut;
                card.mode = mode;
                card.seed = (unsigned)(cut * 7 + mode + sc * 1000);
                memset(consumed, 0, sizeof(consumed));
                fault_scenario(sc);
                TEST_ASSERT_TRUE_MESSAGE(card.off, msg);
                remember_consumed();
                int made = produced;
                /* reboot */
                card.off = false;
                card.cut_at = -1;
                boot(fault_geometry[sc][0], fault_geometry[sc][1], 0x77);
                produced = made;
                until_ready();
                TEST_ASSERT_TRUE_MESSAGE(sp.ok, msg);
                replay_reset();
                replay(-1, msg);
                steps_all();
                int lost = 0;
                for (int c = 0; c < made; c++) {
                    lost += card.durable[c] && !card.overwritten[c] && !consumed[c] && !rp.seen[c];
                }
                int bound = card.faulty_blocks * (int)GW_SFSPILL_BLK_RECS;
                TEST_ASSERT_TRUE_MESSAGE(lost <= bound, msg);
                int dmax = (card.cut_super ? 2 : 1) * (int)GW_SFSPILL_CKPT_RECS;
                TEST_ASSERT_TRUE_MESSAGE(rp.dups <= dmax, msg);
                TEST_ASSERT_TRUE_MESSAGE(sp.holes <= (uint32_t)card.faulty_blocks, msg);
                runs++;
                with_holes += sp.holes > 0;
                with_dups += rp.dups > 0;
                max_dups = rp.dups > max_dups ? rp.dups : max_dups;
                max_lost = lost > max_lost ? lost : max_lost;
            }
        }
        printf("fault test %d: %d runs, %d with holes, %d with duplicates (at most %d samples), "
               "at most %d samples lost\n",
               sc, runs, with_holes, with_dups, max_dups, max_lost);
        TEST_ASSERT_EQUAL_INT(F_COUNT * total, runs);
        TEST_ASSERT_TRUE(with_holes > 0);   /* the damage faults do make holes */
        TEST_ASSERT_TRUE(with_dups > 0);
    }
}


/* ---- #136: a planned restart moves the RAM ring to the card first ---- */

static int flush_ticks, flush_sleeps, tick_changes;

/* The spill's tick (the watchdog feed): after each card operation, at most one change. */
static void flush_tick(void)
{
    TEST_ASSERT_TRUE(card.changes - tick_changes <= 1);
    tick_changes = card.changes;
    flush_ticks++;
}

static uint32_t flush_clock(void)
{
    return now_ms;
}

static void flush_sleep(uint32_t ms)
{
    TEST_ASSERT_TRUE(ms > 0 && ms <= 10);
    now_ms += ms;
    flush_sleeps++;
}

static gw_sf_flush_t flush_now(uint32_t limit_ms)
{
    gw_sf_flush_t res;
    uint32_t op_ms = card.op_ms;
    card.op_ms = op_ms > 0 ? op_ms : 1;   /* time always passes on a real card */
    flush_ticks = flush_sleeps = 0;
    tick_changes = card.changes;
    sp.tick = flush_tick;
    gw_sf_flush(&sf, flush_clock, flush_sleep, limit_ms, &res);
    sp.tick = NULL;
    card.op_ms = op_ms;
    return res;
}

/* Samples below three quarters of the RAM ring (never spilled), a planned restart, a boot: all
 * the samples come back once, in order, after those that were on the card already; a second
 * boot sends nothing again. Each step is one card operation with a tick after it. */
static void test_flush_before_restart(void)
{
    card_new(4096);
    boot(2, 8, 0x136);
    until_ready();
    push_n(300);   /* 300 + 18 strings * 2 continuations = 336 records, below 384 */
    TEST_ASSERT_EQUAL_UINT32(0, sp.blocks);
    TEST_ASSERT_EQUAL_size_t(336, sf.count);
    uint32_t sb_before = sp.sb_last_ms;
    TEST_ASSERT_TRUE(now_ms - sb_before < GW_SFSPILL_SB_GAP_MS);
    gw_sf_flush_t res = flush_now(5000);
    TEST_ASSERT_EQUAL_INT(GW_SF_FLUSH_DONE, res.result);
    TEST_ASSERT_EQUAL_UINT32(300, res.moved);
    TEST_ASSERT_EQUAL_UINT32(336, res.records);
    TEST_ASSERT_EQUAL_UINT32(0, res.left);
    TEST_ASSERT_EQUAL_size_t(0, sf.count);
    TEST_ASSERT_EQUAL_UINT32(2, res.blocks);   /* 169 records, then the rest */
    TEST_ASSERT_EQUAL_UINT32(sp.blocks, res.blocks);
    TEST_ASSERT_TRUE(flush_ticks >= (int)res.steps);
    /* the head and position superblock waited for the 500 ms gap after the boot's one */
    TEST_ASSERT_TRUE(flush_sleeps > 0);
    TEST_ASSERT_TRUE(sp.sb_last_ms - sb_before >= GW_SFSPILL_SB_GAP_MS);
    TEST_ASSERT_TRUE(now_ms - sp.sb_last_ms <= 1);   /* the last step: that superblock */
    TEST_ASSERT_EQUAL_UINT64(sp.head, sp.hint_at);
    /* a second flush has nothing to do */
    int changes = card.changes;
    res = flush_now(5000);
    TEST_ASSERT_EQUAL_INT(GW_SF_FLUSH_DONE, res.result);
    TEST_ASSERT_EQUAL_UINT32(0, res.moved + res.steps);
    TEST_ASSERT_EQUAL_INT(changes, card.changes);

    boot(2, 8, 0x136);   /* the restart: the RAM ring is gone */
    until_ready();
    TEST_ASSERT_EQUAL_UINT64(336, sp.kept_at_boot);
    memset(consumed, 0, sizeof(consumed));
    replay_reset();
    TEST_ASSERT_EQUAL_INT(300, replay(-1, "after the flush"));
    for (int c = 0; c < 300; c++) {
        TEST_ASSERT_TRUE(rp.seen[c]);
    }
    steps_all();   /* the checkpoint after the replay */
    remember_consumed();
    boot(2, 8, 0x136);
    until_ready();
    replay_reset();
    TEST_ASSERT_EQUAL_INT(0, replay(-1, "nothing again"));

    /* samples on the card already and in the RAM ring: the order holds across both */
    card_new(4096);
    boot(2, 8, 0x137);
    until_ready();
    push_n(1000);
    TEST_ASSERT_TRUE(sp.blocks > 0);
    TEST_ASSERT_TRUE(sf.count > 0);
    size_t ram = gw_sf_ram_samples(&sf);
    res = flush_now(5000);
    TEST_ASSERT_EQUAL_INT(GW_SF_FLUSH_DONE, res.result);
    TEST_ASSERT_EQUAL_UINT32(ram, res.moved);
    boot(2, 8, 0x137);
    until_ready();
    memset(consumed, 0, sizeof(consumed));
    replay_reset();
    TEST_ASSERT_EQUAL_INT(1000, replay(-1, "card and RAM"));
}

/* A slow card: the time limit stops the flush (at most one card operation late); the samples
 * moved come back once and in order, the rest is counted as left (lost by the restart), and
 * nothing is replayed from it. */
static void test_flush_time_limit(void)
{
    card_new(4096);
    boot(1, 8, 0x138);
    until_ready();
    now_ms += 1000;   /* no gap wait in this test */
    push_n(370);      /* 370 + 22 * 2 = 414 records: one block spills at 384 */
    TEST_ASSERT_EQUAL_UINT32(1, sp.blocks);
    size_t ram = gw_sf_ram_samples(&sf);
    card.op_ms = 300;
    uint32_t t0 = now_ms;
    /* the extent of the next block, then one block: the limit expires before the second */
    gw_sf_flush_t res = flush_now(500);
    TEST_ASSERT_EQUAL_INT(GW_SF_FLUSH_TIMEOUT, res.result);
    TEST_ASSERT_TRUE(now_ms - t0 < 500 + card.op_ms);
    TEST_ASSERT_EQUAL_UINT32(1, res.blocks);
    TEST_ASSERT_TRUE(res.moved > 0);
    TEST_ASSERT_TRUE(res.left > 0);
    TEST_ASSERT_EQUAL_UINT32(ram, res.moved + res.left);
    TEST_ASSERT_EQUAL_size_t(res.left, gw_sf_ram_samples(&sf));
    int on_card = 0;
    for (int c = 0; c < produced; c++) {
        on_card += card.durable[c];
    }
    card.op_ms = 0;
    boot(1, 8, 0x138);
    until_ready();
    memset(consumed, 0, sizeof(consumed));
    replay_reset();
    int n = replay(-1, "after the time limit");
    TEST_ASSERT_EQUAL_INT(on_card, n);
    TEST_ASSERT_EQUAL_INT(produced - (int)res.left, n);
    for (int c = 0; c < produced; c++) {
        TEST_ASSERT_EQUAL(c < n, rp.seen[c]);   /* the oldest, without a gap */
    }
}

/* The card fails during the flush (a torn block write, then no power to the card): the flush
 * stops at once, the spill is failed, nothing of the torn block is replayed after the boot. A
 * flush of a failed spill, or before the boot recovery ends, does nothing. */
static void test_flush_card_failure(void)
{
    card_new(4096);
    boot(1, 8, 0x139);
    until_ready();
    now_ms += 1000;
    push_n(300);
    size_t ram = gw_sf_ram_samples(&sf);
    card.mode = F_TORN;
    card.cut_at = card.changes + 1;   /* the extent, then the first block is torn */
    card.seed = 7;
    gw_sf_flush_t res = flush_now(5000);
    TEST_ASSERT_EQUAL_INT(GW_SF_FLUSH_FAILED, res.result);
    TEST_ASSERT_FALSE(sp.ok);
    TEST_ASSERT_EQUAL_UINT32(2, res.steps);
    TEST_ASSERT_EQUAL_UINT32(0, res.moved);
    TEST_ASSERT_EQUAL_UINT32(ram, res.left);
    /* a failed spill: nothing */
    int changes = card.changes;
    res = flush_now(5000);
    TEST_ASSERT_EQUAL_INT(GW_SF_FLUSH_NO_SPILL, res.result);
    TEST_ASSERT_EQUAL_UINT32(ram, res.left);
    TEST_ASSERT_EQUAL_INT(changes, card.changes);
    /* the power comes back: the boot finds no valid block */
    card.off = false;
    card.cut_at = -1;
    boot(1, 8, 0x139);
    res = flush_now(5000);   /* the recovery has not run */
    TEST_ASSERT_EQUAL_INT(GW_SF_FLUSH_NOT_READY, res.result);
    TEST_ASSERT_EQUAL_INT(changes, card.changes);
    until_ready();
    memset(consumed, 0, sizeof(consumed));
    replay_reset();
    TEST_ASSERT_EQUAL_INT(0, replay(-1, "after the torn flush"));

    /* without a spill */
    gw_sf_init(&sf, storage, 8);
    gw_sf_push(&sf, 1, 2, 3, false, 4);
    gw_sf_flush(&sf, flush_clock, flush_sleep, 5000, &res);
    TEST_ASSERT_EQUAL_INT(GW_SF_FLUSH_NO_SPILL, res.result);
    TEST_ASSERT_EQUAL_UINT32(1, res.left);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_batches_come_out_in_order);
    RUN_TEST(test_overflow_keeps_the_newest);
    RUN_TEST(test_partly_overwritten_batch);
    RUN_TEST(test_without_storage);
    RUN_TEST(test_64bit_and_strings);
    RUN_TEST(test_overwritten_string_head);
    RUN_TEST(test_crc_and_plan);
    RUN_TEST(test_spill_and_replay);
    RUN_TEST(test_spill_wrap_keeps_newest);
    RUN_TEST(test_spill_reboot);
    RUN_TEST(test_spill_config_change);
    RUN_TEST(test_spill_geometry_change);
    RUN_TEST(test_spill_card_full);
    RUN_TEST(test_spill_superblocks_lost);
    RUN_TEST(test_spill_hole);
    RUN_TEST(test_spill_migration);
    RUN_TEST(test_spill_io_error);
    RUN_TEST(test_spill_faults);
    RUN_TEST(test_flush_before_restart);
    RUN_TEST(test_flush_time_limit);
    RUN_TEST(test_flush_card_failure);
    return UNITY_END();
}
