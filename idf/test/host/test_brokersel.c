/* QA-1: broker list with failover (MQTT-6) and the "every broker failed" signal used by the
 * interface selection (NET-4, issue #57). */
#include "gw_brokersel.h"
#include "unity.h"

static gw_brokersel_t s;

void setUp(void) {}
void tearDown(void) {}

/* One broker (the MVP): never moves, the round fails after failover_after attempts. */
static void test_single_broker(void)
{
    gw_brokersel_init(&s, 1, 3, 0);
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_FALSE(gw_brokersel_failed(&s));
        TEST_ASSERT_EQUAL_UINT8(0, s.current);
        TEST_ASSERT_EQUAL(i >= 2, gw_brokersel_round_failed(&s));
    }
    gw_brokersel_stable(&s, 1000);
    TEST_ASSERT_FALSE(gw_brokersel_round_failed(&s));
    TEST_ASSERT_FALSE(gw_brokersel_should_return(&s, 1u << 30));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, gw_brokersel_next_check_ms(&s, 1000));
}

/* Three brokers, failover after 2: 0, 0, 1, 1, 2, 2, 0 ... in order, wrapping around. */
static void test_failover_in_order_and_wrap(void)
{
    gw_brokersel_init(&s, 3, 2, 0);
    TEST_ASSERT_EQUAL_UINT8(0, s.current);
    TEST_ASSERT_FALSE(gw_brokersel_failed(&s));
    TEST_ASSERT_TRUE(gw_brokersel_failed(&s));
    TEST_ASSERT_EQUAL_UINT8(1, s.current);
    TEST_ASSERT_FALSE(gw_brokersel_failed(&s));
    TEST_ASSERT_FALSE(gw_brokersel_round_failed(&s));
    TEST_ASSERT_TRUE(gw_brokersel_failed(&s));
    TEST_ASSERT_EQUAL_UINT8(2, s.current);
    TEST_ASSERT_FALSE(gw_brokersel_failed(&s));
    TEST_ASSERT_FALSE(gw_brokersel_round_failed(&s));
    TEST_ASSERT_TRUE(gw_brokersel_failed(&s));
    TEST_ASSERT_EQUAL_UINT8(0, s.current);
    TEST_ASSERT_TRUE(gw_brokersel_round_failed(&s));   /* 3 brokers x 2 attempts */

    /* a stable session keeps the broker that works and clears the round */
    TEST_ASSERT_FALSE(gw_brokersel_failed(&s));
    TEST_ASSERT_TRUE(gw_brokersel_failed(&s));
    TEST_ASSERT_EQUAL_UINT8(1, s.current);
    gw_brokersel_stable(&s, 5000);
    TEST_ASSERT_FALSE(gw_brokersel_round_failed(&s));
    gw_brokersel_session_down(&s);
    TEST_ASSERT_FALSE(gw_brokersel_failed(&s));   /* failures count again from 0 */
    TEST_ASSERT_EQUAL_UINT8(1, s.current);
}

/* Default policy (primary_return_ms = 0): a working backup is kept for ever. */
static void test_no_return_by_default(void)
{
    gw_brokersel_init(&s, 2, 1, 0);
    TEST_ASSERT_TRUE(gw_brokersel_failed(&s));
    TEST_ASSERT_EQUAL_UINT8(1, s.current);
    gw_brokersel_stable(&s, 0);
    TEST_ASSERT_FALSE(gw_brokersel_should_return(&s, UINT64_MAX / 2));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, gw_brokersel_next_check_ms(&s, 0));
}

/* With primary_return_ms: after that long on a backup, one try on the primary; if it fails,
 * straight back to the first backup (not failover_after attempts on a dead primary). */
static void test_return_to_primary(void)
{
    gw_brokersel_init(&s, 3, 3, 600000);
    for (int i = 0; i < 3; i++) {
        gw_brokersel_failed(&s);
    }
    TEST_ASSERT_EQUAL_UINT8(1, s.current);
    TEST_ASSERT_FALSE(gw_brokersel_should_return(&s, 0));   /* no stable session yet */
    gw_brokersel_stable(&s, 10000);
    TEST_ASSERT_EQUAL_UINT32(600000, gw_brokersel_next_check_ms(&s, 10000));
    TEST_ASSERT_FALSE(gw_brokersel_should_return(&s, 609999));
    TEST_ASSERT_EQUAL_UINT32(1, gw_brokersel_next_check_ms(&s, 609999));
    TEST_ASSERT_TRUE(gw_brokersel_should_return(&s, 610000));
    gw_brokersel_return_to_primary(&s);
    TEST_ASSERT_EQUAL_UINT8(0, s.current);
    TEST_ASSERT_FALSE(gw_brokersel_should_return(&s, 2000000));
    /* the primary is still down: a single failed try sends the walk to the first backup */
    TEST_ASSERT_TRUE(gw_brokersel_failed(&s));
    TEST_ASSERT_EQUAL_UINT8(1, s.current);
    TEST_ASSERT_FALSE(gw_brokersel_failed(&s));   /* normal counting again */
    TEST_ASSERT_FALSE(gw_brokersel_failed(&s));
    TEST_ASSERT_TRUE(gw_brokersel_failed(&s));
    TEST_ASSERT_EQUAL_UINT8(2, s.current);

    /* the primary is back: the session there is kept, no further return checks */
    gw_brokersel_stable(&s, 3000000);
    TEST_ASSERT_TRUE(gw_brokersel_should_return(&s, 3600000));
    gw_brokersel_return_to_primary(&s);
    gw_brokersel_stable(&s, 3600100);
    TEST_ASSERT_EQUAL_UINT8(0, s.current);
    TEST_ASSERT_FALSE(gw_brokersel_should_return(&s, 9000000));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, gw_brokersel_next_check_ms(&s, 9000000));
}

/* Out-of-range arguments are clamped. */
static void test_clamping(void)
{
    gw_brokersel_init(&s, 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(1, s.count);
    TEST_ASSERT_EQUAL_UINT8(1, s.failover_after);
    gw_brokersel_init(&s, 9, 1, 0);
    TEST_ASSERT_EQUAL_UINT8(GW_MAX_BROKERS, s.count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_single_broker);
    RUN_TEST(test_failover_in_order_and_wrap);
    RUN_TEST(test_no_return_by_default);
    RUN_TEST(test_return_to_primary);
    RUN_TEST(test_clamping);
    return UNITY_END();
}
