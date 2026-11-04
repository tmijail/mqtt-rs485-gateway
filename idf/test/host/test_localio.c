/* QA-1: policy of the local outputs (IO-3 safe state) and of the pulse counters (IO-4). */
#include <string.h>

#include "gw_config.h"
#include "gw_localio.h"
#include "unity.h"

static gw_config_t cfg;

void setUp(void)
{
    memset(&cfg, 0, sizeof(cfg));
}
void tearDown(void) {}

static void modes(const char *m)   /* one letter per DO: o off, 1 on, h hold */
{
    for (int i = 0; i < GW_LOCAL_IO_CHANNELS; i++) {
        cfg.local_io.do_failsafe[i] = m[i] == '1' ? GW_DO_SAFE_ON
                                      : m[i] == 'h' ? GW_DO_SAFE_HOLD : GW_DO_SAFE_OFF;
    }
}

static void test_boot_state_per_output(void)
{
    modes("o1hho1oh");
    gw_lio_t l;
    gw_lio_init(&l, &cfg, 0);
    TEST_ASSERT_EQUAL_HEX8(0x8C, gw_lio_hold_mask(&cfg));
    TEST_ASSERT_EQUAL_HEX8(0x22, l.safe_on);
    TEST_ASSERT_EQUAL_HEX8(0x51, l.safe_off);
    /* the expander kept DO3 and DO8 on (held); DO1 came back on from nowhere: off */
    TEST_ASSERT_EQUAL_HEX8(0xA6, gw_lio_boot_state(&l, 0x85));
    TEST_ASSERT_EQUAL_HEX8(0x22, gw_lio_boot_state(&l, 0x00));   /* power-up: nothing held */
}

static void test_defaults_off_and_no_failsafe(void)
{
    gw_lio_t l;
    gw_lio_init(&l, &cfg, 0);
    TEST_ASSERT_EQUAL_HEX8(0x00, gw_lio_hold_mask(&cfg));
    TEST_ASSERT_EQUAL_HEX8(0x00, gw_lio_boot_state(&l, 0xFF));   /* MVP: everything off */
    uint8_t mask, value;
    for (uint32_t t = 0; t < 10u * 86400000u; t += 3600000u) {
        TEST_ASSERT_EQUAL(GW_LIO_FS_NONE, gw_lio_failsafe(&l, false, t, &mask, &value));
    }
}

static void test_failsafe_after_n_seconds_without_session(void)
{
    modes("1ohhhhhh");
    cfg.local_io.failsafe_s = 30;
    gw_lio_t l;
    uint8_t mask, value;
    gw_lio_init(&l, &cfg, 1000);
    /* from the boot: no session yet */
    TEST_ASSERT_EQUAL(GW_LIO_FS_NONE, gw_lio_failsafe(&l, false, 30999, &mask, &value));
    TEST_ASSERT_EQUAL(GW_LIO_FS_TRIP, gw_lio_failsafe(&l, false, 31000, &mask, &value));
    TEST_ASSERT_EQUAL_HEX8(0x03, mask);
    TEST_ASSERT_EQUAL_HEX8(0x01, value);
    TEST_ASSERT_EQUAL(GW_LIO_FS_NONE, gw_lio_failsafe(&l, false, 90000, &mask, &value)); /* once */
    TEST_ASSERT_EQUAL(GW_LIO_FS_CLEAR, gw_lio_failsafe(&l, true, 95000, &mask, &value));
    TEST_ASSERT_EQUAL(GW_LIO_FS_NONE, gw_lio_failsafe(&l, true, 96000, &mask, &value));
    /* session lost at 200 s: a short outage does not trip, a long one does */
    TEST_ASSERT_EQUAL(GW_LIO_FS_NONE, gw_lio_failsafe(&l, false, 200000, &mask, &value));
    TEST_ASSERT_EQUAL(GW_LIO_FS_NONE, gw_lio_failsafe(&l, false, 229000, &mask, &value));
    TEST_ASSERT_EQUAL(GW_LIO_FS_NONE, gw_lio_failsafe(&l, true, 229500, &mask, &value));
    TEST_ASSERT_EQUAL(GW_LIO_FS_NONE, gw_lio_failsafe(&l, false, 240000, &mask, &value));
    TEST_ASSERT_EQUAL(GW_LIO_FS_NONE, gw_lio_failsafe(&l, false, 269999, &mask, &value));
    TEST_ASSERT_EQUAL(GW_LIO_FS_TRIP, gw_lio_failsafe(&l, false, 270000, &mask, &value));
}

static void test_failsafe_all_hold_and_clock_wrap(void)
{
    modes("hhhhhhhh");
    cfg.local_io.failsafe_s = 5;
    gw_lio_t l;
    uint8_t mask = 1, value = 1;
    gw_lio_init(&l, &cfg, UINT32_MAX - 1000u);   /* the 32-bit ms clock wraps meanwhile */
    TEST_ASSERT_EQUAL(GW_LIO_FS_NONE, gw_lio_failsafe(&l, false, 3000, &mask, &value));
    TEST_ASSERT_EQUAL(GW_LIO_FS_TRIP, gw_lio_failsafe(&l, false, 4000, &mask, &value));
    TEST_ASSERT_EQUAL_HEX8(0, mask);              /* tripped, but nothing to drive */
    TEST_ASSERT_EQUAL_HEX8(0, value);
}

/* IO-4: the counters are saved every counter_save_s, only when they changed */
static void test_counter_save_schedule(void)
{
    cfg.local_io.counter_save_s = 600;
    gw_lio_save_t sv;
    uint64_t c[8] = {3};
    gw_lio_save_init(&sv, &cfg, c, 1000);
    TEST_ASSERT_FALSE(gw_lio_save_due(&sv, c, 1000 + 700000, false));   /* unchanged */
    TEST_ASSERT_FALSE(gw_lio_save_due(&sv, c, 1000, true));
    c[0] = 4;
    TEST_ASSERT_FALSE(gw_lio_save_due(&sv, c, 1000 + 599999, false));
    TEST_ASSERT_TRUE(gw_lio_save_due(&sv, c, 1000 + 599999, true));     /* orderly restart */
    TEST_ASSERT_TRUE(gw_lio_save_due(&sv, c, 1000 + 600000, false));
    gw_lio_saved(&sv, c, 601000);
    TEST_ASSERT_FALSE(gw_lio_save_due(&sv, c, 2000000, false));
    c[7] = 1;
    TEST_ASSERT_FALSE(gw_lio_save_due(&sv, c, 1200999, false));
    TEST_ASSERT_TRUE(gw_lio_save_due(&sv, c, 1201000, false));

    cfg.local_io.counter_save_s = 0;                                    /* never */
    gw_lio_save_init(&sv, &cfg, (const uint64_t[8]){0}, 0);
    TEST_ASSERT_FALSE(gw_lio_save_due(&sv, c, 4000000000u, false));
    TEST_ASSERT_FALSE(gw_lio_save_due(&sv, c, 1, true));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_boot_state_per_output);
    RUN_TEST(test_defaults_off_and_no_failsafe);
    RUN_TEST(test_failsafe_after_n_seconds_without_session);
    RUN_TEST(test_failsafe_all_hold_and_clock_wrap);
    RUN_TEST(test_counter_save_schedule);
    return UNITY_END();
}
