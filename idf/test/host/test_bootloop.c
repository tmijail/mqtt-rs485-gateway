/* FW-1 (issue #144 item 4): the boot-loop counter and safe mode. */
#include <string.h>

#include "gw_bootloop.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_a_bad_record_starts_at_zero(void)
{
    gw_bootloop_t r;
    memset(&r, 0xA5, sizeof(r));   /* RTC memory after a power-on, or of another image */
    TEST_ASSERT_EQUAL_UINT32(0, gw_bootloop_count(&r));
    TEST_ASSERT_EQUAL_UINT32(1, gw_bootloop_boot(&r, GW_RESET_PANIC, false));
    TEST_ASSERT_EQUAL_UINT32(GW_BOOTLOOP_MAGIC, r.magic);
    /* a changed count with the old CRC: bad */
    r.count = 7;
    TEST_ASSERT_EQUAL_UINT32(0, gw_bootloop_count(&r));
    TEST_ASSERT_EQUAL_UINT32(0, gw_bootloop_boot(&r, GW_RESET_SOFTWARE, false));
}

/* +1 for each crash reason; three in a row start safe mode */
static void test_crashes_count_up_to_safe_mode(void)
{
    const gw_reset_kind_t crash[] = {GW_RESET_PANIC, GW_RESET_INT_WDT, GW_RESET_TASK_WDT,
                                     GW_RESET_WDT, GW_RESET_CPU_LOCKUP};
    for (size_t i = 0; i < sizeof(crash) / sizeof(crash[0]); i++) {
        gw_bootloop_t r;
        gw_bootloop_set(&r, 0);
        TEST_ASSERT_EQUAL_UINT32(1, gw_bootloop_boot(&r, crash[i], false));
        TEST_ASSERT_FALSE(gw_bootloop_safe(1, false));
        TEST_ASSERT_EQUAL_UINT32(2, gw_bootloop_boot(&r, crash[i], false));
        TEST_ASSERT_FALSE(gw_bootloop_safe(2, false));
        uint32_t n = gw_bootloop_boot(&r, crash[i], false);
        TEST_ASSERT_EQUAL_UINT32(GW_BOOTLOOP_SAFE, n);
        TEST_ASSERT_TRUE(gw_bootloop_safe(n, false));
        TEST_ASSERT_TRUE(gw_bootloop_safe(n + 1, false));
    }
}

/* power-on, brownout and a power glitch set it to 0; a planned restart and the reset button
 * keep it, so safe mode survives them */
static void test_reset_reasons(void)
{
    const gw_reset_kind_t zero[] = {GW_RESET_POWERON, GW_RESET_BROWNOUT, GW_RESET_PWR_GLITCH};
    for (size_t i = 0; i < sizeof(zero) / sizeof(zero[0]); i++) {
        gw_bootloop_t r;
        gw_bootloop_set(&r, 5);
        TEST_ASSERT_EQUAL_UINT32(0, gw_bootloop_boot(&r, zero[i], false));
        TEST_ASSERT_EQUAL_UINT32(0, gw_bootloop_count(&r));
    }
    const gw_reset_kind_t keep[] = {GW_RESET_SOFTWARE, GW_RESET_EXTERNAL, GW_RESET_OTHER};
    for (size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
        gw_bootloop_t r;
        gw_bootloop_set(&r, 3);
        TEST_ASSERT_EQUAL_UINT32(3, gw_bootloop_boot(&r, keep[i], false));
        TEST_ASSERT_EQUAL_UINT32(3, gw_bootloop_count(&r));
    }
}

/* a pending image ignores the counter: no count, never safe mode (a power-on still clears it) */
static void test_pending_image_ignores_it(void)
{
    gw_bootloop_t r;
    gw_bootloop_set(&r, 2);
    TEST_ASSERT_EQUAL_UINT32(2, gw_bootloop_boot(&r, GW_RESET_PANIC, true));
    TEST_ASSERT_FALSE(gw_bootloop_safe(5, true));
    TEST_ASSERT_EQUAL_UINT32(0, gw_bootloop_boot(&r, GW_RESET_POWERON, true));
}

/* cleared after 120 s in normal mode or by the operator; safe mode retries with N - 1 */
static void test_clear_and_retry(void)
{
    gw_bootloop_t r;
    gw_bootloop_set(&r, GW_BOOTLOOP_SAFE);
    gw_bootloop_set(&r, GW_BOOTLOOP_SAFE - 1);   /* the end of safe mode after 60 min */
    TEST_ASSERT_EQUAL_UINT32(GW_BOOTLOOP_SAFE - 1, gw_bootloop_boot(&r, GW_RESET_SOFTWARE, false));
    TEST_ASSERT_FALSE(gw_bootloop_safe(GW_BOOTLOOP_SAFE - 1, false));
    /* one more crash: safe mode again at once */
    TEST_ASSERT_TRUE(gw_bootloop_safe(gw_bootloop_boot(&r, GW_RESET_PANIC, false), false));
    gw_bootloop_set(&r, 0);
    TEST_ASSERT_EQUAL_UINT32(0, gw_bootloop_count(&r));
    TEST_ASSERT_EQUAL_UINT32(1, gw_bootloop_boot(&r, GW_RESET_TASK_WDT, false));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_bad_record_starts_at_zero);
    RUN_TEST(test_crashes_count_up_to_safe_mode);
    RUN_TEST(test_reset_reasons);
    RUN_TEST(test_pending_image_ignores_it);
    RUN_TEST(test_clear_and_retry);
    return UNITY_END();
}
