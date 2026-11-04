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

#define SEGS 16
typedef struct {
    uint8_t data[SEGS][4096];
    int32_t size[SEGS];   /* -1 = absent */
    bool fail;
    int removes;
} mem_io_t;
static mem_io_t mem;

static bool m_append(void *ctx, uint32_t seg, const void *d, size_t len)
{
    mem_io_t *m = ctx;
    if (m->fail || seg >= SEGS || (m->size[seg] < 0 ? 0 : m->size[seg]) + (int32_t)len > 4096) {
        return false;
    }
    if (m->size[seg] < 0) {
        m->size[seg] = 0;
    }
    memcpy(m->data[seg] + m->size[seg], d, len);
    m->size[seg] += (int32_t)len;
    return true;
}

static bool m_read(void *ctx, uint32_t seg, uint32_t off, void *d, size_t len)
{
    mem_io_t *m = ctx;
    if (m->fail || seg >= SEGS || m->size[seg] < 0 || off + len > (uint32_t)m->size[seg]) {
        return false;
    }
    memcpy(d, m->data[seg] + off, len);
    return true;
}

static bool m_remove(void *ctx, uint32_t seg)
{
    mem_io_t *m = ctx;
    if (seg < SEGS) {
        m->removes += m->size[seg] >= 0;
        m->size[seg] = -1;
    }
    return true;
}

static int32_t m_size(void *ctx, uint32_t seg)
{
    mem_io_t *m = ctx;
    return seg < SEGS ? m->size[seg] : -1;
}

static size_t m_list(void *ctx, uint32_t *ids, size_t max)
{
    mem_io_t *m = ctx;
    size_t n = 0;
    for (uint32_t i = 0; i < SEGS && n < max; i++) {
        if (m->size[i] >= 0) {
            ids[n++] = i;
        }
    }
    return n;
}

static const gw_sfspill_io_t io = {&mem, m_append, m_read, m_remove, m_size, m_list};

static void mem_reset(void)
{
    memset(&mem, 0, sizeof(mem));
    for (int i = 0; i < SEGS; i++) {
        mem.size[i] = -1;
    }
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
 * first, then the ring: nothing lost, in order; replayed segments are deleted. */
static void test_spill_and_replay(void)
{
    mem_reset();
    gw_sfspill_t sp;
    gw_sfspill_init(&sp, &io, 10, 8, 0x1234);
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
    TEST_ASSERT_EQUAL_INT(5, mem.removes);
    /* and again, the segment numbers go on */
    push_range(100, 120);
    TEST_ASSERT_EQUAL_INT(20, replay_all(100));
}

/* The card is bounded too: when all segments are full the oldest one is deleted and counted. */
static void test_spill_bounded(void)
{
    mem_reset();
    gw_sfspill_t sp;
    gw_sfspill_init(&sp, &io, 10, 3, 0x1234);   /* 30 records at most */
    gw_sf_attach_spill(&sf, &sp, 4);
    push_range(0, 60);   /* 52 spilled: 30 fit */
    TEST_ASSERT_EQUAL_UINT32(3, sp.nsegs);
    TEST_ASSERT_TRUE(sp.dropped > 0);
    uint64_t kept = sp.count;
    TEST_ASSERT_EQUAL_size_t(60 - sp.dropped, gw_sf_count(&sf));
    gw_sf_rec_t out[8];
    gw_sf_peek(&sf, out, 8);
    TEST_ASSERT_EQUAL_DOUBLE(60 - 8 - (double)kept, out[0].v.num);   /* the oldest kept */
    TEST_ASSERT_EQUAL_INT((int)(kept + 8), replay_all(60 - 8 - (int)kept));
}

/* A string sample is never split between the spill and the ring, nor between segments. */
static void test_spill_strings(void)
{
    mem_reset();
    gw_sfspill_t sp;
    gw_sfspill_init(&sp, &io, 5, 8, 7);
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
 * configuration (or another format) are deleted. */
static void test_spill_survives_reboot(void)
{
    mem_reset();
    gw_sfspill_t sp;
    gw_sfspill_init(&sp, &io, 10, 8, 0xAAAA);
    gw_sf_attach_spill(&sf, &sp, 4);
    push_range(0, 30);   /* 6 chunks of 4 spilled: segments 0, 1 and 2 */
    TEST_ASSERT_EQUAL_UINT64(24, sp.count);
    /* reboot: the RAM ring is gone, the card is not */
    gw_sf_init(&sf, storage, 8);
    gw_sfspill_init(&sp, &io, 10, 8, 0xAAAA);
    gw_sf_attach_spill(&sf, &sp, 4);
    TEST_ASSERT_EQUAL_UINT32(24, sp.kept_at_boot);
    TEST_ASSERT_EQUAL_INT(24, replay_all(0));
    /* another configuration: the old segments are deleted at boot */
    push_range(0, 30);
    gw_sf_init(&sf, storage, 8);
    gw_sfspill_init(&sp, &io, 10, 8, 0xBBBB);
    TEST_ASSERT_EQUAL_UINT64(0, sp.count);
    int32_t left = 0;
    for (int i = 0; i < SEGS; i++) {
        left += mem.size[i] >= 0;
    }
    TEST_ASSERT_EQUAL_INT32(0, left);
    /* a partial last record (power cut) is ignored */
    gw_sf_attach_spill(&sf, &sp, 4);
    push_range(0, 13);   /* 8 spilled into a new segment (ids go on) */
    uint32_t seg = sp.last;
    mem.size[seg] -= 5;
    gw_sf_init(&sf, storage, 8);
    gw_sfspill_init(&sp, &io, 10, 8, 0xBBBB);
    TEST_ASSERT_EQUAL_UINT64(7, sp.count);
}

/* A card that fails: the spill stops and the ring overwrites as without one. */
static void test_spill_io_error(void)
{
    mem_reset();
    gw_sfspill_t sp;
    gw_sfspill_init(&sp, &io, 10, 8, 1);
    gw_sf_attach_spill(&sf, &sp, 4);
    push_range(0, 12);   /* 4 spilled */
    mem.fail = true;
    push_range(12, 20);
    TEST_ASSERT_FALSE(sp.ok);
    TEST_ASSERT_EQUAL_UINT32(8, sf.dropped);
    gw_sf_rec_t out[8];
    TEST_ASSERT_EQUAL_size_t(1, gw_sf_peek(&sf, out, 8));   /* from the ring */
    TEST_ASSERT_EQUAL_DOUBLE(12, out[0].v.num);
    TEST_ASSERT_EQUAL_UINT32(0x811C9DC5u, gw_sfspill_fnv(GW_SFSPILL_FNV_INIT, "", 0));
    TEST_ASSERT_EQUAL_UINT32(0xE40C292Cu, gw_sfspill_fnv(GW_SFSPILL_FNV_INIT, "a", 1));
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
    return UNITY_END();
}
