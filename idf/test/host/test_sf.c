/* QA-1: store & forward ring (SPB-14). The node-level behaviour (historical DDATA/NDATA after
 * the births) is in test_node. */
#include "gw_sf.h"
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
    TEST_ASSERT_EQUAL_DOUBLE(20.0, out[1].value);
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
    TEST_ASSERT_TRUE(out[0].is_null);
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
        TEST_ASSERT_EQUAL_DOUBLE((double)i, out[0].value);
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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_batches_come_out_in_order);
    RUN_TEST(test_overflow_keeps_the_newest);
    RUN_TEST(test_partly_overwritten_batch);
    RUN_TEST(test_without_storage);
    return UNITY_END();
}
