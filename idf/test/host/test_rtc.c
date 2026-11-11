/* QA-1: PCF85063 register image <-> UTC (TIME-2): BCD, OS bit, plausibility. */
#include <string.h>
#include <time.h>

#include "gw_clock.h"
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

/* Issue #160: the sources of the clock, which of them make it valid, and their order. */
static void test_clock_sources(void)
{
    TEST_ASSERT_FALSE(gw_clock_valid(GW_CLOCK_NONE));
    TEST_ASSERT_TRUE(gw_clock_valid(GW_CLOCK_RTC));
    TEST_ASSERT_TRUE(gw_clock_valid(GW_CLOCK_BROWSER));
    TEST_ASSERT_TRUE(gw_clock_valid(GW_CLOCK_SNTP));
    TEST_ASSERT_EQUAL_STRING("none", gw_clock_source_key(GW_CLOCK_NONE));
    TEST_ASSERT_EQUAL_STRING("rtc", gw_clock_source_key(GW_CLOCK_RTC));
    TEST_ASSERT_EQUAL_STRING("browser", gw_clock_source_key(GW_CLOCK_BROWSER));
    TEST_ASSERT_EQUAL_STRING("sntp", gw_clock_source_key(GW_CLOCK_SNTP));
    TEST_ASSERT_EQUAL(GW_CLOCK_BROWSER, gw_clock_after(GW_CLOCK_NONE, GW_CLOCK_BROWSER));
    TEST_ASSERT_EQUAL(GW_CLOCK_BROWSER, gw_clock_after(GW_CLOCK_RTC, GW_CLOCK_BROWSER));
    TEST_ASSERT_EQUAL(GW_CLOCK_SNTP, gw_clock_after(GW_CLOCK_BROWSER, GW_CLOCK_SNTP));
    TEST_ASSERT_EQUAL(GW_CLOCK_SNTP, gw_clock_after(GW_CLOCK_SNTP, GW_CLOCK_BROWSER));
    TEST_ASSERT_EQUAL(GW_CLOCK_RTC, gw_clock_after(GW_CLOCK_RTC, GW_CLOCK_NONE));
}

static gw_clock_set_t set(const char *body, gw_clock_source_t cur, int64_t *ms, char *why)
{
    return gw_clock_parse_set(body, strlen(body), cur, ms, why, 160);
}

/* Issue #160: POST /api/time takes {"utc_ms": <integer>} in the range of the RTC, and does not
 * change a clock that SNTP synchronises. */
static void test_clock_parse_set(void)
{
    char why[160];
    int64_t ms = 0;
    TEST_ASSERT_EQUAL(GW_CLOCK_SET_OK, set("{\"utc_ms\": 1793799462123}", GW_CLOCK_NONE, &ms, why));
    TEST_ASSERT_EQUAL_INT64(1793799462123LL, ms);
    TEST_ASSERT_EQUAL(GW_CLOCK_SET_OK, set("{\"utc_ms\":1735689600000}", GW_CLOCK_RTC, &ms, why));
    TEST_ASSERT_EQUAL_INT64(GW_RTC_MIN_EPOCH * 1000LL, ms);
    TEST_ASSERT_EQUAL(GW_CLOCK_SET_OK, set("{\"utc_ms\":4102444799999}", GW_CLOCK_BROWSER, &ms, why));
    TEST_ASSERT_TRUE(gw_clock_plausible_ms(GW_RTC_MAX_EPOCH * 1000LL + 999));
    TEST_ASSERT_FALSE(gw_clock_plausible_ms(GW_RTC_MAX_EPOCH * 1000LL + 1000));
    TEST_ASSERT_FALSE(gw_clock_plausible_ms(GW_RTC_MIN_EPOCH * 1000LL - 1));

    ms = 42;
    static const char *const bad[] = {
        "",                                       /* not JSON */
        "[1793799462123]",                        /* not an object */
        "{}",                                     /* no key */
        "{\"utc_s\": 1793799462}",                /* other key */
        "{\"utc_ms\": 1793799462123, \"x\": 1}",  /* one more key */
        "{\"utc_ms\": \"1793799462123\"}",        /* a string */
        "{\"utc_ms\": 1793799462123.5}",          /* not an integer */
        "{\"utc_ms\": 1735689599999}",            /* before 2025 */
        "{\"utc_ms\": 5000}",                     /* 1970 */
        "{\"utc_ms\": -1}",
        "{\"utc_ms\": 4102444800000}",            /* after 2099 */
        "{\"utc_ms\": 1e300}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        why[0] = '\0';
        TEST_ASSERT_EQUAL_MESSAGE(GW_CLOCK_SET_BAD, set(bad[i], GW_CLOCK_NONE, &ms, why), bad[i]);
        TEST_ASSERT_NOT_EQUAL(0, strlen(why));
    }
    TEST_ASSERT_EQUAL_INT64(42, ms);   /* not changed */
    TEST_ASSERT_EQUAL(GW_CLOCK_SET_SYNCED,
                      set("{\"utc_ms\": 1793799462123}", GW_CLOCK_SNTP, &ms, why));
    TEST_ASSERT_NOT_NULL(strstr(why, "SNTP"));
    TEST_ASSERT_EQUAL(GW_CLOCK_SET_BAD, set("{}", GW_CLOCK_SNTP, &ms, why));   /* bad first */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bcd);
    RUN_TEST(test_decode_and_encode);
    RUN_TEST(test_round_trip_against_libc);
    RUN_TEST(test_rejected_readings);
    RUN_TEST(test_clock_sources);
    RUN_TEST(test_clock_parse_set);
    return UNITY_END();
}
