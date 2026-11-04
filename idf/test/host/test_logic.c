/* QA-1: report by exception / deadband, debounce, reconnect backoff, offline detection. */
#include <math.h>
#include <stdint.h>

#include "gw_backoff.h"
#include "gw_debounce.h"
#include "gw_devstate.h"
#include "gw_rbe.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_rbe_first_report_and_exact_change(void)
{
    gw_rbe_t s = {0};
    TEST_ASSERT_TRUE(gw_rbe_changed(&s, 1.0, true, 0));
    gw_rbe_commit(&s, 1.0, true);
    TEST_ASSERT_FALSE(gw_rbe_changed(&s, 1.0, true, 0));
    TEST_ASSERT_TRUE(gw_rbe_changed(&s, 1.0000001, true, 0));
}

static void test_rbe_deadband_against_last_reported(void)
{
    gw_rbe_t s = {0};
    gw_rbe_commit(&s, 230.0, true);
    TEST_ASSERT_FALSE(gw_rbe_changed(&s, 230.05, true, 0.1));
    TEST_ASSERT_FALSE(gw_rbe_changed(&s, 229.95, true, 0.1));
    TEST_ASSERT_FALSE(gw_rbe_changed(&s, 230.1, true, 0.1));  /* strictly greater than deadband */
    TEST_ASSERT_TRUE(gw_rbe_changed(&s, 230.11, true, 0.1));
    TEST_ASSERT_TRUE(gw_rbe_changed(&s, 229.8, true, 0.1));
    /* slow drift: each sample below the band, but the reference does not move */
    double v = 230.0;
    int reports = 0;
    for (int i = 0; i < 10; i++) {
        v += 0.03;
        if (gw_rbe_changed(&s, v, true, 0.1)) {
            gw_rbe_commit(&s, v, true);
            reports++;
        }
    }
    TEST_ASSERT_EQUAL(2, reports);  /* at +0.12 and +0.24 */
}

static void test_rbe_validity_and_nan(void)
{
    gw_rbe_t s = {0};
    gw_rbe_commit(&s, 5.0, true);
    TEST_ASSERT_TRUE(gw_rbe_changed(&s, 5.0, false, 0));
    gw_rbe_commit(&s, 5.0, false);
    TEST_ASSERT_FALSE(gw_rbe_changed(&s, 7.0, false, 0));  /* still invalid: nothing new */
    TEST_ASSERT_TRUE(gw_rbe_changed(&s, 5.0, true, 0));
    gw_rbe_commit(&s, NAN, true);
    TEST_ASSERT_FALSE(gw_rbe_changed(&s, NAN, true, 0.5));
    TEST_ASSERT_TRUE(gw_rbe_changed(&s, 1.0, true, 0.5));
}

static void test_debounce(void)
{
    gw_debounce_t d;
    gw_debounce_init(&d, 20, 0x00);
    for (int i = 0; i < 19; i++) {
        TEST_ASSERT_EQUAL_HEX8(0, gw_debounce_sample(&d, 0x04));
    }
    TEST_ASSERT_EQUAL_HEX8(0x04, gw_debounce_sample(&d, 0x04));
    TEST_ASSERT_EQUAL_HEX8(0x04, d.stable);
    /* a glitch shorter than the threshold is filtered */
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL_HEX8(0, gw_debounce_sample(&d, 0x00));
    }
    TEST_ASSERT_EQUAL_HEX8(0, gw_debounce_sample(&d, 0x04));
    for (int i = 0; i < 19; i++) {
        TEST_ASSERT_EQUAL_HEX8(0, gw_debounce_sample(&d, 0x00));
    }
    TEST_ASSERT_EQUAL_HEX8(0x04, gw_debounce_sample(&d, 0x00));
    TEST_ASSERT_EQUAL_HEX8(0x00, d.stable);
    gw_debounce_init(&d, 0, 0xFF);  /* 0 ms: every sample is accepted */
    TEST_ASSERT_EQUAL_HEX8(0x81, gw_debounce_sample(&d, 0x7E));
}

static void test_backoff(void)
{
    TEST_ASSERT_EQUAL(1000, gw_backoff_ms(1, 1000, 60000, 0));
    TEST_ASSERT_EQUAL(1000, gw_backoff_ms(1, 1000, 60000, 250)); /* clamped to the minimum */
    TEST_ASSERT_EQUAL(4000, gw_backoff_ms(3, 1000, 60000, 0));
    TEST_ASSERT_EQUAL(3000, gw_backoff_ms(3, 1000, 60000, 1000));
    for (uint32_t a = 1; a < 40; a++) {
        for (uint32_t r = 0; r < 100000; r += 7919) {
            uint32_t d = gw_backoff_ms(a, 1000, 60000, r);
            TEST_ASSERT_TRUE(d >= 1000 && d <= 60000);
        }
    }
    /* jitter still applies at the cap */
    TEST_ASSERT_EQUAL(60000, gw_backoff_ms(30, 1000, 60000, 0));
    TEST_ASSERT_EQUAL(45000, gw_backoff_ms(30, 1000, 60000, 15000));
}

static void test_devstate(void)
{
    gw_devstate_t s = {0};
    TEST_ASSERT_EQUAL(GW_DEV_NO_CHANGE, gw_devstate_cycle(&s, false, 3)); /* starts offline */
    TEST_ASSERT_EQUAL(GW_DEV_WENT_ONLINE, gw_devstate_cycle(&s, true, 3));
    TEST_ASSERT_EQUAL(GW_DEV_NO_CHANGE, gw_devstate_cycle(&s, false, 3));
    TEST_ASSERT_EQUAL(GW_DEV_NO_CHANGE, gw_devstate_cycle(&s, false, 3));
    TEST_ASSERT_EQUAL(GW_DEV_NO_CHANGE, gw_devstate_cycle(&s, true, 3));  /* streak reset */
    TEST_ASSERT_EQUAL(GW_DEV_NO_CHANGE, gw_devstate_cycle(&s, false, 3));
    TEST_ASSERT_EQUAL(GW_DEV_NO_CHANGE, gw_devstate_cycle(&s, false, 3));
    TEST_ASSERT_EQUAL(GW_DEV_WENT_OFFLINE, gw_devstate_cycle(&s, false, 3));
    TEST_ASSERT_EQUAL(GW_DEV_NO_CHANGE, gw_devstate_cycle(&s, false, 3));
    TEST_ASSERT_EQUAL(GW_DEV_WENT_ONLINE, gw_devstate_cycle(&s, true, 3));
}

/* MB-6: exact report by exception for 64-bit integers and strings. */
static void test_rbe_exact_keys(void)
{
    gw_rbe_t s = {0};
    const uint64_t big = 9007199254740993ull;   /* 2^53 + 1 */
    TEST_ASSERT_TRUE(gw_rbe_changed_key(&s, big, false, true, 0));
    gw_rbe_commit_key(&s, big, true);
    TEST_ASSERT_FALSE(gw_rbe_changed_key(&s, big, false, true, 0));
    TEST_ASSERT_TRUE(gw_rbe_changed_key(&s, big + 1, false, true, 0));  /* same double */
    TEST_ASSERT_TRUE(gw_rbe_changed_key(&s, big, false, false, 0));     /* validity */
    /* signed distance across zero and the deadband */
    gw_rbe_commit_key(&s, (uint64_t)(int64_t)-3, true);
    TEST_ASSERT_FALSE(gw_rbe_changed_key(&s, 2, true, true, 5));
    TEST_ASSERT_TRUE(gw_rbe_changed_key(&s, 3, true, true, 5));
    TEST_ASSERT_TRUE(gw_rbe_changed_key(&s, (uint64_t)INT64_MIN, true, true, 5));
    /* unsigned: 2 vs 2^64 - 3 is far apart */
    TEST_ASSERT_TRUE(gw_rbe_changed_key(&s, 2, false, true, 5));
    TEST_ASSERT_NOT_EQUAL(gw_rbe_hash("AB"), gw_rbe_hash("BA"));
    TEST_ASSERT_EQUAL_UINT64(gw_rbe_hash(""), 1469598103934665603ull);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_rbe_first_report_and_exact_change);
    RUN_TEST(test_rbe_exact_keys);
    RUN_TEST(test_rbe_deadband_against_last_reported);
    RUN_TEST(test_rbe_validity_and_nan);
    RUN_TEST(test_debounce);
    RUN_TEST(test_backoff);
    RUN_TEST(test_devstate);
    return UNITY_END();
}
