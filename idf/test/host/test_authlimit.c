/* QA-1: login rate limit per source address (issue #64). */
#include "gw_authlimit.h"
#include "unity.h"

static gw_authlimit_t l;

void setUp(void)
{
    gw_authlimit_init(&l, 5, 30000, 120000);
}

void tearDown(void) {}

static void ip(uint8_t out[16], uint8_t last)
{
    memset(out, 0, 16);
    out[0] = 10;
    out[3] = last;
}

/* 5 failures -> blocked 30 s; even the right password is not checked while blocked; the next
 * block in a row doubles, up to the maximum; a success resets everything. */
static void test_block_and_backoff(void)
{
    uint8_t a[16];
    ip(a, 2);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT32(0, gw_authlimit_check(&l, a, 1000));
        TEST_ASSERT_EQUAL_UINT32(0, gw_authlimit_failed(&l, a, 1000));
    }
    TEST_ASSERT_EQUAL_UINT32(30000, gw_authlimit_failed(&l, a, 1000));
    TEST_ASSERT_EQUAL_UINT32(30000, gw_authlimit_check(&l, a, 1000));
    TEST_ASSERT_EQUAL_UINT32(1, gw_authlimit_check(&l, a, 30999));
    TEST_ASSERT_EQUAL_UINT32(0, gw_authlimit_check(&l, a, 31000));
    TEST_ASSERT_EQUAL_UINT32(2, l.refused_total);
    TEST_ASSERT_EQUAL_UINT32(1, gw_authlimit_blocked(&l, 1000));
    TEST_ASSERT_EQUAL_UINT32(0, gw_authlimit_blocked(&l, 31000));
    /* five more failures: 60 s, then 120 s, then capped at 120 s */
    uint32_t expect[] = {60000, 120000, 120000};
    uint64_t t = 31000;
    for (int b = 0; b < 3; b++) {
        for (int i = 0; i < 4; i++) {
            TEST_ASSERT_EQUAL_UINT32(0, gw_authlimit_failed(&l, a, t));
        }
        TEST_ASSERT_EQUAL_UINT32(expect[b], gw_authlimit_failed(&l, a, t));
        t += expect[b];
    }
    TEST_ASSERT_EQUAL_UINT32(4, l.blocks_total);
    TEST_ASSERT_EQUAL_UINT32(20, l.failed_total);
    /* a success forgets the address: the next block starts again at 30 s */
    gw_authlimit_succeeded(&l, a);
    for (int i = 0; i < 4; i++) {
        gw_authlimit_failed(&l, a, t);
    }
    TEST_ASSERT_EQUAL_UINT32(30000, gw_authlimit_failed(&l, a, t));
}

/* Addresses are independent; a full table reuses the least recently seen unblocked entry and
 * never forgets a blocked address while an unblocked one can go. */
static void test_addresses_and_table(void)
{
    uint8_t a[16], b[16];
    ip(a, 2);
    ip(b, 3);
    for (int i = 0; i < 5; i++) {
        gw_authlimit_failed(&l, a, 100);
    }
    TEST_ASSERT_NOT_EQUAL(0, gw_authlimit_check(&l, a, 200));
    TEST_ASSERT_EQUAL_UINT32(0, gw_authlimit_check(&l, b, 200));
    /* fill the rest of the table with addresses that failed once */
    for (uint8_t i = 10; i < 10 + GW_AUTHLIMIT_SLOTS + 4; i++) {
        uint8_t x[16];
        ip(x, i);
        gw_authlimit_failed(&l, x, 300u + i);
    }
    TEST_ASSERT_NOT_EQUAL(0, gw_authlimit_check(&l, a, 400));   /* still blocked */
    /* every entry blocked: the least recently seen one goes */
    gw_authlimit_init(&l, 1, 1000, 1000);
    for (uint8_t i = 0; i < GW_AUTHLIMIT_SLOTS; i++) {
        uint8_t x[16];
        ip(x, i);
        TEST_ASSERT_EQUAL_UINT32(1000, gw_authlimit_failed(&l, x, 10u + i));
    }
    uint8_t n[16];
    ip(n, 200);
    TEST_ASSERT_EQUAL_UINT32(1000, gw_authlimit_failed(&l, n, 20));
    uint8_t first[16];
    ip(first, 0);
    TEST_ASSERT_EQUAL_UINT32(0, gw_authlimit_check(&l, first, 21));   /* evicted */
    TEST_ASSERT_NOT_EQUAL(0, gw_authlimit_check(&l, n, 21));
}

/* Issue #110: the web server's stall watchdog. */
static void test_webstall(void)
{
    TEST_ASSERT_FALSE(gw_webstall_stuck(1000, 0, false, 8000));
    TEST_ASSERT_FALSE(gw_webstall_stuck(8000, 0, false, 8000));     /* exactly the limit */
    TEST_ASSERT_TRUE(gw_webstall_stuck(8001, 0, false, 8000));
    TEST_ASSERT_FALSE(gw_webstall_stuck(60000, 0, true, 8000));     /* a handler or a drain */
    /* the 32-bit millisecond clock wraps after 49 days */
    TEST_ASSERT_FALSE(gw_webstall_stuck(100, 0xFFFFFF00u, false, 8000));
    TEST_ASSERT_TRUE(gw_webstall_stuck(9000, 0xFFFFFF00u, false, 8000));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_block_and_backoff);
    RUN_TEST(test_addresses_and_table);
    RUN_TEST(test_webstall);
    return UNITY_END();
}
