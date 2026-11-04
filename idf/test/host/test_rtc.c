/* QA-1: PCF85063 register image <-> UTC (TIME-2): BCD, OS bit, plausibility. */
#include <string.h>
#include <time.h>

#include "gw_rtc.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_bcd(void)
{
    TEST_ASSERT_EQUAL_UINT8(0x59, gw_bin_to_bcd(59));
    TEST_ASSERT_EQUAL_UINT8(59, gw_bcd_to_bin(0x59));
    for (uint8_t i = 0; i < 100; i++) {
        TEST_ASSERT_EQUAL_UINT8(i, gw_bcd_to_bin(gw_bin_to_bcd(i)));
    }
}

/* 2026-11-04T13:37:42Z, a Wednesday */
static const uint8_t k_regs[GW_RTC_REGS] = {0x42, 0x37, 0x13, 0x04, 0x03, 0x11, 0x26};
#define K_EPOCH 1793799462LL

static void test_decode_and_encode(void)
{
    int64_t t = 0;
    TEST_ASSERT_EQUAL(GW_RTC_OK, gw_rtc_decode(k_regs, &t));
    TEST_ASSERT_EQUAL_INT64(K_EPOCH, t);
    uint8_t r[GW_RTC_REGS];
    TEST_ASSERT_TRUE(gw_rtc_encode(K_EPOCH, r));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_regs, r, GW_RTC_REGS);
    /* the unused bits of each register are ignored (century, clock-integrity flags) */
    uint8_t noisy[GW_RTC_REGS];
    memcpy(noisy, k_regs, sizeof(noisy));
    noisy[1] |= 0x80;
    noisy[2] |= 0xC0;
    noisy[4] |= 0xF8;
    TEST_ASSERT_EQUAL(GW_RTC_OK, gw_rtc_decode(noisy, &t));
    TEST_ASSERT_EQUAL_INT64(K_EPOCH, t);
}

/* Every day of 2025..2099 (leap years, month ends) against the C library. */
static void test_round_trip_against_libc(void)
{
    for (int64_t t = GW_RTC_MIN_EPOCH; t <= GW_RTC_MAX_EPOCH; t += 86400 + 3601) {
        uint8_t r[GW_RTC_REGS];
        TEST_ASSERT_TRUE(gw_rtc_encode(t, r));
        int64_t back = 0;
        TEST_ASSERT_EQUAL(GW_RTC_OK, gw_rtc_decode(r, &back));
        TEST_ASSERT_EQUAL_INT64(t, back);
        time_t tt = (time_t)t;
        struct tm tm;
        gmtime_r(&tt, &tm);
        TEST_ASSERT_EQUAL_UINT8(tm.tm_mday, gw_bcd_to_bin(r[3]));
        TEST_ASSERT_EQUAL_UINT8(tm.tm_wday, r[4]);
        TEST_ASSERT_EQUAL_UINT8(tm.tm_mon + 1, gw_bcd_to_bin(r[5]));
        TEST_ASSERT_EQUAL_UINT8(tm.tm_year - 100, gw_bcd_to_bin(r[6]));
    }
}

static void test_rejected_readings(void)
{
    int64_t t = 7;
    uint8_t r[GW_RTC_REGS];
    memcpy(r, k_regs, sizeof(r));
    r[0] |= GW_RTC_OS_BIT;                       /* the oscillator stopped */
    TEST_ASSERT_EQUAL(GW_RTC_STOPPED, gw_rtc_decode(r, &t));
    memcpy(r, k_regs, sizeof(r));
    r[1] = 0x5A;                                 /* not BCD */
    TEST_ASSERT_EQUAL(GW_RTC_INVALID, gw_rtc_decode(r, &t));
    memcpy(r, k_regs, sizeof(r));
    r[3] = 0x31;                                 /* 31 November */
    TEST_ASSERT_EQUAL(GW_RTC_INVALID, gw_rtc_decode(r, &t));
    memcpy(r, k_regs, sizeof(r));
    r[5] = 0x02;
    r[3] = 0x29;
    r[6] = 0x27;                                 /* 29 February 2027 */
    TEST_ASSERT_EQUAL(GW_RTC_INVALID, gw_rtc_decode(r, &t));
    r[6] = 0x28;                                 /* 2028 is a leap year */
    TEST_ASSERT_EQUAL(GW_RTC_OK, gw_rtc_decode(r, &t));
    memcpy(r, k_regs, sizeof(r));
    r[2] = 0x24;                                 /* hour 24 */
    TEST_ASSERT_EQUAL(GW_RTC_INVALID, gw_rtc_decode(r, &t));
    const uint8_t reset[GW_RTC_REGS] = {0x00, 0x00, 0x00, 0x01, 0x06, 0x01, 0x00};
    t = 7;
    TEST_ASSERT_EQUAL(GW_RTC_IMPLAUSIBLE, gw_rtc_decode(reset, &t));   /* 2000-01-01 */
    TEST_ASSERT_EQUAL_INT64(7, t);                                     /* untouched */
    /* an unset system clock (1970) or one past the chip's range is never stored */
    TEST_ASSERT_FALSE(gw_rtc_encode(12345, r));
    TEST_ASSERT_FALSE(gw_rtc_encode(GW_RTC_MAX_EPOCH + 1, r));
    TEST_ASSERT_TRUE(gw_rtc_encode(GW_RTC_MAX_EPOCH, r));
    TEST_ASSERT_EQUAL_UINT8(0x99, r[6]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bcd);
    RUN_TEST(test_decode_and_encode);
    RUN_TEST(test_round_trip_against_libc);
    RUN_TEST(test_rejected_readings);
    return UNITY_END();
}
