/* Issue #207: the identify pattern (LED and buzzer) and its time. */
#include "gw_ident.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_start_runs_ten_seconds(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, gw_ident_left_ms(0, 1234));   /* none ever started */
    uint32_t until = gw_ident_start(0, 1000);
    TEST_ASSERT_EQUAL_UINT32(11000, until);
    TEST_ASSERT_EQUAL_UINT32(GW_IDENT_MS, gw_ident_left_ms(until, 1000));
    TEST_ASSERT_EQUAL_UINT32(10, gw_ident_left_s(until, 1000));
    TEST_ASSERT_EQUAL_UINT32(7, gw_ident_left_s(until, 4001));   /* rounded up: 6.999 s */
    TEST_ASSERT_EQUAL_UINT32(1, gw_ident_left_s(until, 10999));
    TEST_ASSERT_EQUAL_UINT32(0, gw_ident_left_ms(until, 11000));
    TEST_ASSERT_EQUAL_UINT32(0, gw_ident_left_s(until, 50000));
}

/* a second request during the 10 s does not start the time again (#207: the button stays
 * disabled); after the end a new one starts */
static void test_second_request_refused(void)
{
    uint32_t until = gw_ident_start(0, 5000);
    TEST_ASSERT_EQUAL_UINT32(0, gw_ident_start(until, 9000));
    TEST_ASSERT_EQUAL_UINT32(0, gw_ident_start(until, 14999));
    TEST_ASSERT_EQUAL_UINT32(25000, gw_ident_start(until, 15000));
}

/* the 32-bit ms clock wraps after 49 days: the time left stays correct across it, an end of
 * exactly 0 is never returned (0 means none), and a stale end does not count as running */
static void test_clock_wrap(void)
{
    uint32_t now = 0xFFFFF000u;
    uint32_t until = gw_ident_start(0, now);
    TEST_ASSERT_EQUAL_UINT32(now + GW_IDENT_MS, until);
    TEST_ASSERT_EQUAL_UINT32(GW_IDENT_MS - 0x2000u, gw_ident_left_ms(until, now + 0x2000u));
    TEST_ASSERT_EQUAL_UINT32(1u, gw_ident_start(0, 0u - GW_IDENT_MS));
    TEST_ASSERT_EQUAL_UINT32(0, gw_ident_left_ms(20000, 20000u + 0x80000000u));
    TEST_ASSERT_EQUAL_UINT32(0, gw_ident_left_ms(60000, 1000));   /* more than 10 s ahead */
}

/* red, green and blue in turn, 250 ms each (4 Hz); a 150 ms beep every 500 ms */
static void test_pattern(void)
{
    TEST_ASSERT_EQUAL(GW_IDENT_RED, gw_ident_color(0));
    TEST_ASSERT_EQUAL(GW_IDENT_RED, gw_ident_color(249));
    TEST_ASSERT_EQUAL(GW_IDENT_GREEN, gw_ident_color(250));
    TEST_ASSERT_EQUAL(GW_IDENT_BLUE, gw_ident_color(500));
    TEST_ASSERT_EQUAL(GW_IDENT_RED, gw_ident_color(750));
    TEST_ASSERT_EQUAL(GW_IDENT_RED, gw_ident_color(9999));   /* 39 steps */
    unsigned on = 0;
    for (uint32_t t = 0; t < GW_IDENT_MS; t += 10) {
        on += gw_ident_tone(t);
    }
    TEST_ASSERT_EQUAL_UINT(20 * 15, on);   /* 20 beeps of 150 ms in 10 s */
    TEST_ASSERT_TRUE(gw_ident_tone(0));
    TEST_ASSERT_FALSE(gw_ident_tone(150));
    TEST_ASSERT_TRUE(gw_ident_tone(500));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_start_runs_ten_seconds);
    RUN_TEST(test_second_request_refused);
    RUN_TEST(test_clock_wrap);
    RUN_TEST(test_pattern);
    return UNITY_END();
}
