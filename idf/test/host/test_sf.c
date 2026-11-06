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

/* ---- spill to a (simulated) card ---- */

#define SEGS 64
#define SEGB 8192
#define VALS 256   /* the sample values of a scenario: 0 .. VALS - 1 */
typedef struct {
    uint8_t data[SEGS][SEGB];
    int32_t size[SEGS];   /* -1 = absent */
    bool fail;            /* a broken card: every operation fails */
    int removes;
    /* #122: a power cut at one change of the card, and the work of each operation */
    int changes;          /* operations that changed the card */
    int cut_at;           /* the change with this index is cut (-1: none) */
    bool off;             /* the power is off: every operation fails */
    uint32_t max_freed;   /* most allocation units one operation freed */
    uint32_t max_patch;   /* most bytes one in-place write wrote */
    bool durable[VALS];   /* samples written whole by a write that returned true */
    bool consumed[VALS];  /* samples replayed from the spill before the cut */
} mem_io_t;
static mem_io_t mem;

enum { CH_GO, CH_CUT, CH_OFF };

/* One change of the card: CH_CUT is the one the power cut interrupts; after it, CH_OFF. */
static int change(mem_io_t *m)
{
    if (m->fail || m->off) {
        return CH_OFF;
    }
    m->changes++;
    if (m->cut_at >= 0 && m->changes - 1 == m->cut_at) {
        m->off = true;
        return CH_CUT;
    }
    return CH_GO;
}

static gw_sfspill_io_t io;

static uint32_t units(int32_t size)
{
    uint32_t u = io.unit != 0 ? io.unit : 1;
    return size <= 0 ? 0 : ((uint32_t)size + u - 1) / u;
}

static void freed(mem_io_t *m, int32_t from, int32_t to)
{
    uint32_t f = units(from) - units(to);
    m->max_freed = f > m->max_freed ? f : m->max_freed;
}

static bool m_write(void *ctx, uint32_t seg, uint32_t off, const void *d, size_t len)
{
    mem_io_t *m = ctx;
    if (seg >= SEGS || off > (uint32_t)(m->size[seg] < 0 ? 0 : m->size[seg]) || off + len > SEGB) {
        return false;
    }
    int c = change(m);
    if (c == CH_OFF) {
        return false;
    }
    if (c == CH_CUT) {
        len /= 2;   /* torn: the first half reaches the card */
    }
    if (m->size[seg] < 0) {
        m->size[seg] = 0;
    }
    memcpy(m->data[seg] + off, d, len);
    if ((int32_t)(off + len) > m->size[seg]) {
        m->size[seg] = (int32_t)(off + len);
    }
    if (c == CH_GO && off >= sizeof(gw_sfspill_hdr_t)) {
        for (size_t i = 0; i + sizeof(gw_sf_rec_t) <= len; i += sizeof(gw_sf_rec_t)) {
            gw_sf_rec_t r;
            memcpy(&r, (const uint8_t *)d + i, sizeof(r));
            if (r.v.num >= 0 && r.v.num < VALS) {
                m->durable[(int)r.v.num] = true;
            }
        }
    }
    return c == CH_GO;
}

static bool m_patch(void *ctx, uint32_t seg, uint32_t off, const void *d, size_t len)
{
    mem_io_t *m = ctx;
    if (seg >= SEGS || m->size[seg] < 0 || off + len > (uint32_t)m->size[seg]) {
        return false;
    }
    int c = change(m);
    if (c == CH_OFF) {
        return false;
    }
    m->max_patch = len > m->max_patch ? (uint32_t)len : m->max_patch;
    memcpy(m->data[seg] + off, d, c == CH_CUT ? len / 2 : len);
    return c == CH_GO;
}

static bool m_read(void *ctx, uint32_t seg, uint32_t off, void *d, size_t len)
{
    mem_io_t *m = ctx;
    if (m->fail || m->off || seg >= SEGS || m->size[seg] < 0 ||
        off + len > (uint32_t)m->size[seg]) {
        return false;
    }
    memcpy(d, m->data[seg] + off, len);
    return true;
}

/* A cut truncate is lost (FAT writes the directory entry with the fsync, at the end). */
static bool m_truncate(void *ctx, uint32_t seg, uint32_t len)
{
    mem_io_t *m = ctx;
    if (seg >= SEGS || m->size[seg] < 0 || len >= (uint32_t)m->size[seg]) {
        return false;
    }
    if (change(m) != CH_GO) {
        return false;
    }
    freed(m, m->size[seg], (int32_t)len);
    m->size[seg] = (int32_t)len;
    return true;
}

/* A cut unlink still removes the file (FAT removes the directory entry first; the clusters
 * of the step that were not freed yet are lost, which a real card shows as lost clusters). */
static bool m_remove(void *ctx, uint32_t seg)
{
    mem_io_t *m = ctx;
    if (seg >= SEGS || m->size[seg] < 0) {
        return !m->fail && !m->off;
    }
    int c = change(m);
    if (c == CH_OFF) {
        return false;
    }
    freed(m, m->size[seg], 0);
    m->removes++;
    m->size[seg] = -1;
    return c == CH_GO;
}

static int32_t m_size(void *ctx, uint32_t seg)
{
    mem_io_t *m = ctx;
    return !m->off && seg < SEGS ? m->size[seg] : -1;
}

static size_t m_list(void *ctx, uint32_t *ids, size_t max)
{
    mem_io_t *m = ctx;
    size_t n = 0;
    for (uint32_t i = 0; i < SEGS && n < max && !m->off; i++) {
        if (m->size[i] >= 0) {
            ids[n++] = i;
        }
    }
    return n;
}

static uint32_t work[GW_SFSPILL_LIST_MAX];

static void mem_reset_unit(uint32_t unit)
{
    memset(&mem, 0, sizeof(mem));
    for (int i = 0; i < SEGS; i++) {
        mem.size[i] = -1;
    }
    mem.cut_at = -1;
    io = (gw_sfspill_io_t){&mem, unit, m_write, m_patch, m_read, m_truncate, m_remove, m_size, m_list};
}

static void mem_reset(void)
{
    mem_reset_unit(0);
}

static void spill_init(gw_sfspill_t *sp, uint32_t recs, uint32_t segs, uint32_t hash)
{
    gw_sfspill_init(sp, &io, recs, segs, hash, work, GW_SFSPILL_LIST_MAX);
}

static int files(void)
{
    int n = 0;
    for (int i = 0; i < SEGS; i++) {
        n += mem.size[i] >= 0;
    }
    return n;
}

/* Every pending delete, step by step; returns the steps. Each step changes the card at most
 * once (#122: one bounded operation per pass of the Sparkplug loop). */
static int gc_all(gw_sfspill_t *sp)
{
    int steps = 0;
    for (int before = mem.changes; gw_sfspill_step(sp); before = mem.changes) {
        TEST_ASSERT_LESS_OR_EQUAL_INT(1, mem.changes - before);
        TEST_ASSERT_LESS_THAN_INT(1000, ++steps);
    }
    return steps;
}

/* Push one numeric sample per batch, values @p from .. @p to - 1. */
static void push_range(int from, int to)
{
    for (int i = from; i < to; i++) {
        gw_sf_begin(&sf);
        gw_sf_push(&sf, 0, 1, i, false, (uint64_t)i);
    }
}

/* Replay everything; returns how many came out, checks they are consecutive from @p first. */
static int replay_all(int first)
{
    gw_sf_rec_t out[8];
    int n = 0;
    size_t k;
    while ((k = gw_sf_peek(&sf, out, 8)) > 0) {
        for (size_t i = 0; i < k; i++) {
            TEST_ASSERT_EQUAL_DOUBLE(first + n, out[i].v.num);
            n++;
        }
        gw_sf_pop(&sf, k);
    }
    return n;
}

/* The full ring spills its oldest records (a chunk at a time) to the card; they are replayed
 * first, then the ring: nothing lost, in order; replayed segments are deleted (in steps). */
static void test_spill_and_replay(void)
{
    mem_reset();
    gw_sfspill_t sp;
    spill_init(&sp, 10, 8, 0x1234);
    gw_sf_attach_spill(&sf, &sp, 4);
    push_range(0, 50);
    TEST_ASSERT_EQUAL_UINT32(0, sf.dropped);
    TEST_ASSERT_EQUAL_UINT32(44, sf.spilled);   /* 11 chunks of 4: 8 records stay in RAM */
    TEST_ASSERT_EQUAL_UINT64(44, sp.count);
    TEST_ASSERT_EQUAL_size_t(50, gw_sf_count(&sf));
    TEST_ASSERT_EQUAL_UINT32(5, sp.nsegs);       /* 10 records per segment */
    TEST_ASSERT_EQUAL_INT32(16 + 10 * 24, mem.size[0]);
    TEST_ASSERT_EQUAL_INT(50, replay_all(0));
    TEST_ASSERT_EQUAL_UINT64(0, sp.count);
    TEST_ASSERT_EQUAL_UINT32(0, sp.nsegs);
    /* replayed segments are dead at once, deleted by the steps */
    TEST_ASSERT_EQUAL_INT(0, mem.removes);
    TEST_ASSERT_EQUAL_UINT32(5, gw_sfspill_gc_pending(&sp));
    TEST_ASSERT_EQUAL_UINT32(GW_SFSPILL_DEAD, *(uint32_t *)mem.data[0]);
    TEST_ASSERT_EQUAL_INT(5, gc_all(&sp));   /* marked already: one unlink each */
    TEST_ASSERT_EQUAL_INT(5, mem.removes);
    TEST_ASSERT_EQUAL_INT(0, files());
    TEST_ASSERT_EQUAL_UINT32(5, sp.gc_files);
    /* and again, the segment numbers go on */
    push_range(100, 120);
    TEST_ASSERT_EQUAL_INT(20, replay_all(100));
}

/* The card is bounded too: when all segments are full the oldest one leaves and is counted. */
static void test_spill_bounded(void)
{
    mem_reset();
    gw_sfspill_t sp;
    spill_init(&sp, 10, 3, 0x1234);   /* 30 records at most */
    gw_sf_attach_spill(&sf, &sp, 4);
    push_range(0, 60);   /* 52 spilled: 30 fit */
    TEST_ASSERT_EQUAL_UINT32(3, sp.nsegs);
    TEST_ASSERT_TRUE(sp.dropped > 0);
    TEST_ASSERT_TRUE(sp.bytes <= sp.max_bytes);
    uint64_t kept = sp.count;
    TEST_ASSERT_EQUAL_size_t(60 - sp.dropped, gw_sf_count(&sf));
    gw_sf_rec_t out[8];
    gw_sf_peek(&sf, out, 8);
    TEST_ASSERT_EQUAL_DOUBLE(60 - 8 - (double)kept, out[0].v.num);   /* the oldest kept */
    TEST_ASSERT_EQUAL_INT((int)(kept + 8), replay_all(60 - 8 - (int)kept));
    gc_all(&sp);
    TEST_ASSERT_EQUAL_INT(0, files());
}

/* A string sample is never split between the spill and the ring, nor between segments. */
static void test_spill_strings(void)
{
    mem_reset();
    gw_sfspill_t sp;
    spill_init(&sp, 5, 8, 7);
    gw_sf_attach_spill(&sf, &sp, 2);
    for (int i = 0; i < 6; i++) {
        char s[24];
        snprintf(s, sizeof(s), "value-%02d-0123456789", i);   /* 19 bytes: 1 + 3 records */
        gw_sf_begin(&sf);
        gw_sf_push_str(&sf, 0, 3, s, false, (uint64_t)i);
    }
    TEST_ASSERT_EQUAL_UINT32(0, sf.dropped);
    gw_sf_rec_t out[8];
    char s[40];
    for (int i = 0; i < 6; i++) {
        size_t k = gw_sf_peek(&sf, out, 8);
        TEST_ASSERT_EQUAL_size_t(4, k);
        gw_sf_sample(out, k, s, sizeof(s));
        char want[24];
        snprintf(want, sizeof(want), "value-%02d-0123456789", i);
        TEST_ASSERT_EQUAL_STRING(want, s);
        gw_sf_pop(&sf, k);
    }
    TEST_ASSERT_EQUAL_size_t(0, gw_sf_count(&sf));
}

/* After a reboot the segments with the same configuration hash are replayed; files of another
 * configuration (or another format) are deleted, in steps after the boot (#122: the boot only
 * reads). */
static void test_spill_survives_reboot(void)
{
    mem_reset();
    gw_sfspill_t sp;
    spill_init(&sp, 10, 8, 0xAAAA);
    gw_sf_attach_spill(&sf, &sp, 4);
    push_range(0, 30);   /* 6 chunks of 4 spilled: segments 0, 1 and 2 */
    TEST_ASSERT_EQUAL_UINT64(24, sp.count);
    /* reboot: the RAM ring is gone, the card is not */
    gw_sf_init(&sf, storage, 8);
    spill_init(&sp, 10, 8, 0xAAAA);
    gw_sf_attach_spill(&sf, &sp, 4);
    TEST_ASSERT_EQUAL_UINT32(24, sp.kept_at_boot);
    TEST_ASSERT_EQUAL_INT(24, replay_all(0));
    gc_all(&sp);
    /* another configuration: the old segments are not replayed, and are deleted later */
    push_range(0, 30);
    gw_sf_init(&sf, storage, 8);
    int changes = mem.changes;
    spill_init(&sp, 10, 8, 0xBBBB);
    TEST_ASSERT_EQUAL_INT(changes, mem.changes);   /* the boot changes nothing */
    TEST_ASSERT_EQUAL_UINT64(0, sp.count);
    TEST_ASSERT_EQUAL_UINT32(0, sp.trimmed);       /* not this configuration's samples */
    TEST_ASSERT_EQUAL_INT(3, files());
    TEST_ASSERT_EQUAL_UINT32(3, gw_sfspill_gc_pending(&sp));
    TEST_ASSERT_EQUAL_INT(6, gc_all(&sp));         /* each: mark, unlink */
    TEST_ASSERT_EQUAL_INT(0, files());
    /* a partial last record (power cut) is ignored */
    gw_sf_attach_spill(&sf, &sp, 4);
    push_range(0, 13);   /* 8 spilled into a new segment (ids go on) */
    uint32_t seg = sp.last;
    TEST_ASSERT_EQUAL_UINT32(6, seg);
    mem.size[seg] -= 5;
    gw_sf_init(&sf, storage, 8);
    spill_init(&sp, 10, 8, 0xBBBB);
    TEST_ASSERT_EQUAL_UINT64(7, sp.count);
}

/* A card that fails: the spill stops and the ring overwrites as without one. */
static void test_spill_io_error(void)
{
    mem_reset();
    gw_sfspill_t sp;
    spill_init(&sp, 10, 8, 1);
    gw_sf_attach_spill(&sf, &sp, 4);
    push_range(0, 12);   /* 4 spilled */
    mem.fail = true;
    push_range(12, 20);
    TEST_ASSERT_FALSE(sp.ok);
    TEST_ASSERT_EQUAL_UINT32(8, sf.dropped);
    gw_sf_rec_t out[8];
    TEST_ASSERT_EQUAL_size_t(1, gw_sf_peek(&sf, out, 8));   /* from the ring */
    TEST_ASSERT_EQUAL_DOUBLE(12, out[0].v.num);
    TEST_ASSERT_FALSE(gw_sfspill_step(&sp));   /* and no more deletes */
    TEST_ASSERT_EQUAL_UINT32(0x811C9DC5u, gw_sfspill_fnv(GW_SFSPILL_FNV_INIT, "", 0));
    TEST_ASSERT_EQUAL_UINT32(0xE40C292Cu, gw_sfspill_fnv(GW_SFSPILL_FNV_INIT, "a", 1));
}

/* #119: check one layout of gw_sfspill_plan(): its bounds, and that it fills max_kb. */
static void check_plan(uint32_t max_kb, uint32_t want_segs, uint32_t want_records)
{
    uint32_t recs = 0, segs = 0;
    TEST_ASSERT_EQUAL_UINT32(max_kb < GW_SFSPILL_MIN_KB   ? GW_SFSPILL_MIN_KB
                             : max_kb > GW_SFSPILL_MAX_KB ? GW_SFSPILL_MAX_KB
                                                          : max_kb,
                             gw_sfspill_plan(max_kb, 0, &recs, &segs));
    char msg[48];
    snprintf(msg, sizeof(msg), "max_kb %lu", (unsigned long)max_kb);
    if (want_segs != 0) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(want_segs, segs, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(want_records, recs, msg);
    }
    TEST_ASSERT_TRUE_MESSAGE(segs >= GW_SFSPILL_MIN_SEGS && segs <= GW_SFSPILL_MAX_SEGS, msg);
    uint64_t kb = max_kb < GW_SFSPILL_MIN_KB   ? GW_SFSPILL_MIN_KB
                  : max_kb > GW_SFSPILL_MAX_KB ? GW_SFSPILL_MAX_KB
                                               : max_kb;
    uint64_t bytes = kb * 1024u;
    uint64_t seg_bytes = sizeof(gw_sfspill_hdr_t) + (uint64_t)recs * sizeof(gw_sf_rec_t);
    TEST_ASSERT_TRUE_MESSAGE(seg_bytes <= INT32_MAX, msg);              /* io.size() is int32 */
    TEST_ASSERT_TRUE_MESSAGE(segs * seg_bytes <= bytes, msg);           /* within max_kb */
    TEST_ASSERT_TRUE_MESSAGE(bytes - segs * seg_bytes < segs * 25u, msg);   /* and all of it */
    /* the preferred segment size, unless all the segments are in use */
    TEST_ASSERT_TRUE_MESSAGE(recs >= GW_SFSPILL_SEG_RECORDS || segs == GW_SFSPILL_MIN_SEGS, msg);
    /* larger than preferred only to stay within the segment table */
    TEST_ASSERT_TRUE_MESSAGE(recs < GW_SFSPILL_SEG_RECORDS * 2u || segs == GW_SFSPILL_MAX_SEGS,
                             msg);
}

/* #119: the segment layout for sparkplug.store_forward.sd.max_kb. Before, segments stopped at
 * 4096 records, so 128 segments held 12.6 MB at most (the default max_kb is 16 MB), and the
 * 16-byte headers often left 7 segments instead of 8. */
static void test_spill_plan(void)
{
    TEST_ASSERT_EQUAL_size_t(16, sizeof(gw_sfspill_hdr_t));
    TEST_ASSERT_EQUAL_size_t(24, sizeof(gw_sf_rec_t));
    check_plan(GW_SFSPILL_MIN_KB, 8, 340);       /* the minimum: 8 segments (it was 7) */
    check_plan(128, 8, 682);                     /* 128 KB, with no cluster limit */
    check_plan(256, 8, 1364);                    /* it was 7 */
    check_plan(768, 8, 4095);                    /* it was 7 */
    check_plan(1024, 10, 4368);
    check_plan(12288, 127, 4127);
    check_plan(12290, 128, 4096);                /* 128 segments of exactly 4096 */
    check_plan(16384, 128, 5460);                /* the default: 16 MB used, not 12.6 MB */
    check_plan(GW_SFSPILL_MAX_KB, 128, 1398100); /* the maximum: 4 GiB, 32 MiB segments */
    /* out of range (the configuration rejects it): clamped to the bounds */
    check_plan(0, 8, 340);
    check_plan(UINT32_MAX, 128, 1398100);
    /* and every max_kb in between keeps the invariants */
    for (uint32_t kb = GW_SFSPILL_MIN_KB; kb <= 20000; kb++) {
        check_plan(kb, 0, 0);
    }
    for (uint64_t kb = 20000; kb <= GW_SFSPILL_MAX_KB; kb = kb * 9 / 8 + 7) {
        check_plan((uint32_t)kb, 0, 0);
    }
    /* the planned count is the bound gw_sfspill_init() keeps */
    mem_reset();
    gw_sfspill_t sp;
    uint32_t recs, segs;
    gw_sfspill_plan(16384, 0, &recs, &segs);
    spill_init(&sp, recs, segs, 1);
    TEST_ASSERT_EQUAL_UINT32(128, sp.max_segs);
    TEST_ASSERT_EQUAL_UINT32(5460, sp.seg_records);
}

/* #122: one layout with the cluster size @p unit: each segment is at most one delete step. */
static void check_plan_unit(uint32_t max_kb, uint32_t unit, uint32_t want_kb, uint32_t want_segs,
                            uint32_t want_records)
{
    uint32_t recs = 0, segs = 0;
    char msg[64];
    snprintf(msg, sizeof(msg), "max_kb %lu unit %lu", (unsigned long)max_kb, (unsigned long)unit);
    uint32_t kb = gw_sfspill_plan(max_kb, unit, &recs, &segs);
    if (want_segs != 0) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(want_kb, kb, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(want_segs, segs, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(want_records, recs, msg);
    }
    uint64_t seg_bytes = sizeof(gw_sfspill_hdr_t) + (uint64_t)recs * sizeof(gw_sf_rec_t);
    TEST_ASSERT_TRUE_MESSAGE(seg_bytes <= (uint64_t)unit * GW_SFSPILL_STEP_UNITS, msg);
    TEST_ASSERT_TRUE_MESSAGE(segs >= GW_SFSPILL_MIN_SEGS && segs <= GW_SFSPILL_MAX_SEGS, msg);
    TEST_ASSERT_TRUE_MESSAGE(kb <= max_kb || max_kb < GW_SFSPILL_MIN_KB, msg);
    TEST_ASSERT_TRUE_MESSAGE(segs * seg_bytes <= (uint64_t)kb * 1024u, msg);
    TEST_ASSERT_TRUE_MESSAGE((uint64_t)kb * 1024u - segs * seg_bytes < segs * 25u, msg);
    /* limited only when 128 segments of one step do not hold max_kb */
    TEST_ASSERT_TRUE_MESSAGE(kb == max_kb || segs == GW_SFSPILL_MAX_SEGS, msg);
}

/* #122: the segment size comes from the card's cluster size, read at mount. */
static void test_spill_plan_cluster(void)
{
    /* 32 KB clusters (the factory format of SDHC cards): 4 MB per segment, so the default
     * layout does not change, and 4 GiB becomes 128 segments of 4 MB */
    check_plan_unit(16384, 32768, 16384, 128, 5460);
    check_plan_unit(GW_SFSPILL_MAX_KB, 32768, 524288, 128, 174762);
    /* 4 KB clusters: 512 KB per segment, so at most 64 MB */
    check_plan_unit(16384, 4096, 16384, 128, 5460);
    check_plan_unit(GW_SFSPILL_MAX_KB, 4096, 65536, 128, 21844);
    /* 512 B clusters (a small card): 64 KB per segment, so the default holds only 8 MB */
    check_plan_unit(16384, 512, 8192, 128, 2730);
    check_plan_unit(GW_SFSPILL_MIN_KB, 512, 64, 8, 340);
    /* the QEMU stand-in (32 B "clusters", 4 KB steps): the E2E's 128 KB */
    check_plan_unit(128, 32, 128, 32, 170);
    static const uint32_t units_[] = {32, 512, 1024, 4096, 16384, 32768, 65536};
    for (size_t u = 0; u < sizeof(units_) / sizeof(units_[0]); u++) {
        for (uint64_t kb = GW_SFSPILL_MIN_KB; kb <= GW_SFSPILL_MAX_KB; kb = kb * 5 / 4 + 3) {
            check_plan_unit((uint32_t)kb, units_[u], 0, 0, 0);
        }
    }
}

/* #122 problem 1: a smaller max_kb after a reboot. The boot only reads; the oldest segments
 * over the new limit are not replayed, and the steps delete them. */
static void test_spill_trim_at_boot(void)
{
    mem_reset();
    gw_sfspill_t sp;
    spill_init(&sp, 10, 8, 0x77);
    gw_sf_attach_spill(&sf, &sp, 4);
    push_range(0, 88);   /* 80 spilled: 8 full segments */
    TEST_ASSERT_EQUAL_UINT32(8, sp.nsegs);
    TEST_ASSERT_EQUAL_UINT32(0, sp.dropped);
    /* reboot with 4 segments of 10 */
    gw_sf_init(&sf, storage, 8);
    int changes = mem.changes;
    spill_init(&sp, 10, 4, 0x77);
    TEST_ASSERT_EQUAL_INT(changes, mem.changes);   /* not in the boot */
    TEST_ASSERT_EQUAL_UINT32(4, sp.nsegs);
    TEST_ASSERT_EQUAL_UINT32(40, sp.kept_at_boot);
    TEST_ASSERT_EQUAL_UINT32(40, sp.trimmed);
    TEST_ASSERT_TRUE(sp.bytes <= sp.max_bytes);
    TEST_ASSERT_EQUAL_UINT32(4, gw_sfspill_gc_pending(&sp));
    TEST_ASSERT_EQUAL_INT(8, files());
    TEST_ASSERT_EQUAL_INT(8, gc_all(&sp));   /* each: the dead mark, then the unlink */
    TEST_ASSERT_EQUAL_INT(4, files());
    TEST_ASSERT_EQUAL_UINT32(4, sp.gc_files);
    /* reboot with less max_kb (segments of 5: 6 of them hold 816 B, three old ones 768 B) */
    gw_sf_init(&sf, storage, 8);
    spill_init(&sp, 5, 6, 0x77);
    TEST_ASSERT_EQUAL_UINT32(3, sp.nsegs);
    TEST_ASSERT_EQUAL_UINT32(30, sp.kept_at_boot);
    TEST_ASSERT_EQUAL_UINT32(10, sp.trimmed);
    TEST_ASSERT_TRUE(sp.bytes <= sp.max_bytes);
    gc_all(&sp);
    TEST_ASSERT_EQUAL_INT(3, files());
    /* the newest data is what stays */
    gw_sf_attach_spill(&sf, &sp, 4);
    TEST_ASSERT_EQUAL_INT(30, replay_all(50));
    /* new segments follow the new layout */
    push_range(200, 220);
    TEST_ASSERT_EQUAL_UINT32(5, sp.seg_records);
    TEST_ASSERT_EQUAL_INT32(16 + 5 * 24, mem.size[sp.first]);
}

/* #122 problem 2: a segment larger than one step (written before the cluster limit) is deleted
 * in steps of at most 128 allocation units, after the dead mark. */
static void test_spill_delete_steps(void)
{
    mem_reset_unit(8);   /* a step: 128 units of 8 B, 1024 B */
    gw_sfspill_t sp;
    spill_init(&sp, 300, 4, 0x99);   /* the old layout: 7216 B segments */
    gw_sf_attach_spill(&sf, &sp, 4);
    push_range(0, 608);   /* 600 spilled: 2 segments */
    TEST_ASSERT_EQUAL_INT32(16 + 300 * 24, mem.size[0]);
    /* reboot with the layout the cluster size allows: 40 records (976 B) per segment */
    uint32_t recs, segs;
    gw_sfspill_plan(GW_SFSPILL_MIN_KB, 8, &recs, &segs);
    TEST_ASSERT_EQUAL_UINT32(42, recs);
    gw_sf_init(&sf, storage, 8);
    spill_init(&sp, 40, 8, 0x99);   /* 7808 B: one old segment fits, the older one not */
    TEST_ASSERT_EQUAL_UINT32(1024, sp.step_bytes);
    TEST_ASSERT_EQUAL_UINT32(1, sp.nsegs);
    TEST_ASSERT_EQUAL_UINT32(300, sp.trimmed);
    /* phase 1: the mark only */
    TEST_ASSERT_TRUE(gw_sfspill_step(&sp));
    TEST_ASSERT_EQUAL_UINT32(GW_SFSPILL_DEAD, *(uint32_t *)mem.data[0]);
    TEST_ASSERT_EQUAL_INT32(7216, mem.size[0]);
    /* phase 2: 7216 -> 7168, 6144, ... 1024 B, then the unlink */
    TEST_ASSERT_EQUAL_INT32(7168, (gw_sfspill_step(&sp), mem.size[0]));
    TEST_ASSERT_EQUAL_INT32(6144, (gw_sfspill_step(&sp), mem.size[0]));
    TEST_ASSERT_EQUAL_INT(6, gc_all(&sp));
    TEST_ASSERT_EQUAL_INT32(-1, mem.size[0]);
    TEST_ASSERT_EQUAL_UINT32(9, sp.gc_steps);   /* mark, 7 truncates, unlink */
    /* the kept old segment replays, then goes in 8 steps (dead already: no mark step) */
    gw_sf_attach_spill(&sf, &sp, 4);
    TEST_ASSERT_EQUAL_INT(300, replay_all(300));
    TEST_ASSERT_EQUAL_INT(8, gc_all(&sp));
    TEST_ASSERT_EQUAL_INT(0, files());
    /* new segments hold one step at most: one unlink each */
    push_range(1000, 1108);   /* 100 spilled: 3 segments */
    TEST_ASSERT_EQUAL_UINT32(3, sp.nsegs);
    TEST_ASSERT_EQUAL_INT(108, replay_all(1000));
    TEST_ASSERT_EQUAL_INT(3, gc_all(&sp));
    TEST_ASSERT_EQUAL_INT(0, files());
    /* the bound of every operation */
    TEST_ASSERT_TRUE(mem.max_freed <= GW_SFSPILL_STEP_UNITS);
    TEST_ASSERT_EQUAL_UINT32(4, mem.max_patch);
}

/* #122: the scenario of the power-cut test. The power goes off at change @p cut_at (-1: never),
 * and the scenario goes on with a dead card. Returns the changes it made. */
static int cut_scenario(int cut_at, gw_sfspill_t *sp)
{
    mem_reset_unit(1);   /* steps of 128 B: a segment of 10 records (256 B) needs a truncate */
    mem.cut_at = cut_at;
    gw_sf_init(&sf, storage, 8);
    spill_init(sp, 10, 6, 0x5A5A);
    gw_sf_attach_spill(&sf, sp, 4);
    push_range(0, 60);   /* 52 spilled: 6 segments */
    gw_sf_rec_t out[8];
    for (int done = 0; done < 25;) {   /* a replay of 25 samples: segments 0 and 1 dead */
        size_t k = gw_sf_peek(&sf, out, 25 - done < 8 ? (size_t)(25 - done) : 8);
        if (k == 0) {
            break;
        }
        bool spilled = sf.peeked_spill;
        gw_sf_pop(&sf, k);
        for (size_t i = 0; spilled && i < k; i++) {
            mem.consumed[(int)out[i].v.num] = true;
        }
        done += (int)k;
    }
    gc_all(sp);
    push_range(60, 80);   /* the spill goes on */
    gc_all(sp);
    return mem.changes;
}

/* #122: a power cut after each change of the card (a torn write, a lost truncate, an unlink
 * that removed only the directory entry). After the reboot: no garbage and nothing lost of
 * what was written and not replayed, few duplicates, every file that is not live deleted by
 * the steps (nothing leaks), and every step within its bound. */
static void test_spill_delete_cut(void)
{
    gw_sfspill_t sp;
    int total = cut_scenario(-1, &sp);
    TEST_ASSERT_EQUAL_UINT32(0, sp.dropped);
    printf("cut test: %d changes of the card, each one cut\n", total);
    TEST_ASSERT_TRUE(total > 20);
    for (int cut = 0; cut <= total; cut++) {
        cut_scenario(cut, &sp);
        char msg[32];
        snprintf(msg, sizeof(msg), "cut at change %d", cut);
        /* reboot */
        mem.off = false;
        mem.cut_at = -1;
        gw_sf_init(&sf, storage, 8);
        spill_init(&sp, 10, 6, 0x5A5A);
        gw_sf_attach_spill(&sf, &sp, 4);
        gc_all(&sp);
        bool seen[VALS] = {false};
        int last = -1, dups = 0;
        gw_sf_rec_t out[8];
        size_t k;
        while ((k = gw_sf_peek(&sf, out, 8)) > 0) {
            for (size_t i = 0; i < k; i++) {
                const gw_sf_rec_t *r = &out[i];
                TEST_ASSERT_TRUE_MESSAGE(r->v.num >= 0 && r->v.num < 80 && r->dev == 0 &&
                                             r->metric == 1 && r->flags == 0 &&
                                             r->ts_ms == (uint64_t)r->v.num,
                                         msg);   /* no garbage */
                int v = (int)r->v.num;
                TEST_ASSERT_TRUE_MESSAGE(v > last, msg);   /* in order, once */
                last = v;
                seen[v] = true;
                dups += mem.consumed[v];
            }
            gw_sf_pop(&sf, k);
        }
        for (int v = 0; v < 80; v++) {
            if (mem.durable[v] && !mem.consumed[v]) {
                TEST_ASSERT_TRUE_MESSAGE(seen[v], msg);   /* nothing lost */
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(dups <= 2 * 10, msg);   /* at most two segments again */
        gc_all(&sp);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, files(), msg);   /* nothing leaks */
        TEST_ASSERT_TRUE_MESSAGE(mem.max_freed <= GW_SFSPILL_STEP_UNITS, msg);
        TEST_ASSERT_TRUE_MESSAGE(mem.max_patch <= 4, msg);
    }
}

/* Write a segment as the code before #122 did (the same format, version 1): header, records,
 * and @p partial bytes of a record cut by a power loss. */
static void old_segment(uint32_t seg, uint32_t hash, int from, int n, int partial)
{
    gw_sfspill_hdr_t h = {GW_SFSPILL_MAGIC, GW_SFSPILL_VERSION, 24, hash, seg};
    memcpy(mem.data[seg], &h, sizeof(h));
    for (int i = 0; i < n + (partial > 0); i++) {
        gw_sf_rec_t r = {.ts_ms = (uint64_t)(from + i), .metric = 1};
        r.v.num = from + i;
        memcpy(mem.data[seg] + sizeof(h) + (size_t)i * sizeof(r), &r, sizeof(r));
    }
    mem.size[seg] = (int32_t)(sizeof(h) + (size_t)n * sizeof(gw_sf_rec_t)) + partial;
}

/* #122 compatibility: segments that the firmware before #122 left on a card are replayed; the
 * next append overwrites a partial record from its boundary (it appended after it before). */
static void test_spill_old_format(void)
{
    mem_reset_unit(512);
    old_segment(7, 0x31, 0, 10, 0);
    old_segment(8, 0x31, 10, 4, 5);   /* the newest: 4 records and 5 bytes of a fifth */
    old_segment(9, 0x32, 500, 3, 0);  /* another configuration */
    gw_sfspill_t sp;
    gw_sf_init(&sf, storage, 8);
    spill_init(&sp, 10, 8, 0x31);
    TEST_ASSERT_EQUAL_UINT32(14, sp.kept_at_boot);
    TEST_ASSERT_EQUAL_UINT32(10, sp.next_id);
    gw_sf_attach_spill(&sf, &sp, 4);
    push_range(14, 30);   /* 8 spilled: 6 into segment 8, at the record boundary */
    TEST_ASSERT_EQUAL_INT32(16 + 10 * 24, mem.size[8]);
    TEST_ASSERT_EQUAL_INT(30, replay_all(0));
    gc_all(&sp);
    TEST_ASSERT_EQUAL_INT(0, files());
}

/* #122: a torn segment between two live ones (a cut while it was created) is skipped by the
 * replay and deleted. */
static void test_spill_gap(void)
{
    mem_reset();
    old_segment(3, 0x40, 0, 10, 0);
    mem.size[4] = 8;   /* half a header */
    memset(mem.data[4], 0x47, 8);
    old_segment(5, 0x40, 10, 6, 0);
    gw_sfspill_t sp;
    gw_sf_init(&sf, storage, 8);
    spill_init(&sp, 10, 8, 0x40);
    TEST_ASSERT_EQUAL_UINT32(2, sp.nsegs);
    TEST_ASSERT_EQUAL_UINT32(1, gw_sfspill_gc_pending(&sp));
    gw_sf_attach_spill(&sf, &sp, 4);
    TEST_ASSERT_EQUAL_INT(16, replay_all(0));
    gc_all(&sp);
    TEST_ASSERT_EQUAL_INT(0, files());
}

/* #122: with the delete queue full, a replayed segment stays dead on the card (never replayed
 * again); the next boot deletes it. */
static void test_spill_queue_full(void)
{
    mem_reset();
    gw_sfspill_t sp;
    gw_sf_init(&sf, storage, 8);
    gw_sfspill_init(&sp, &io, 10, 8, 0x50, work, 4);
    gw_sf_attach_spill(&sf, &sp, 4);
    push_range(0, 68);   /* 60 spilled: 6 segments */
    TEST_ASSERT_EQUAL_INT(68, replay_all(0));
    TEST_ASSERT_EQUAL_UINT32(4, gw_sfspill_gc_pending(&sp));
    TEST_ASSERT_EQUAL_UINT32(2, sp.gc_left);
    gc_all(&sp);
    TEST_ASSERT_EQUAL_INT(2, files());
    gw_sf_init(&sf, storage, 8);
    spill_init(&sp, 10, 8, 0x50);
    TEST_ASSERT_EQUAL_UINT64(0, sp.count);
    TEST_ASSERT_EQUAL_INT(2, gc_all(&sp));   /* dead already: one unlink each */
    TEST_ASSERT_EQUAL_INT(0, files());
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
    RUN_TEST(test_spill_and_replay);
    RUN_TEST(test_spill_bounded);
    RUN_TEST(test_spill_strings);
    RUN_TEST(test_spill_survives_reboot);
    RUN_TEST(test_spill_io_error);
    RUN_TEST(test_spill_plan);
    RUN_TEST(test_spill_plan_cluster);
    RUN_TEST(test_spill_trim_at_boot);
    RUN_TEST(test_spill_delete_steps);
    RUN_TEST(test_spill_delete_cut);
    RUN_TEST(test_spill_old_format);
    RUN_TEST(test_spill_gap);
    RUN_TEST(test_spill_queue_full);
    return UNITY_END();
}
