/* Issue #267: the entropy sources of the RNG and the order of the boot (gw_rng.h). The posix port
 * records the state of the SAR ADC source (gw_port_rng_sar_state). */
#include <string.h>

#include "gw_port.h"
#include "gw_rng.h"
#include "unity.h"

static char s_log[2048];

void setUp(void)
{
    gw_rng_reset();
    gw_port_log_capture(s_log, sizeof(s_log));
}

void tearDown(void)
{
    gw_port_log_capture(NULL, 0);
}

static bool sar(void)
{
    return gw_port_rng_sar_state(NULL);
}

/* The sequence of the board: the SAR ADC source for the values of the early boot, then the radio
 * for the rest; the source goes off before the Wi-Fi driver starts. */
static void test_the_sequence_of_the_board(void)
{
    TEST_ASSERT_EQUAL(GW_RNG_SRC_NONE, gw_rng_source());
    gw_rng_boot();
    TEST_ASSERT_TRUE(sar());
    TEST_ASSERT_EQUAL(GW_RNG_SRC_SAR_ADC, gw_rng_note("the commissioning password"));
    TEST_ASSERT_EQUAL(GW_RNG_SRC_SAR_ADC, gw_rng_note("the salt of the web password"));
    gw_rng_radio_starts();
    TEST_ASSERT_FALSE(sar());
    gw_rng_radio_on(true);
    TEST_ASSERT_EQUAL(GW_RNG_SRC_RADIO, gw_rng_source());
    gw_rng_adc_starts("the temperature sensor starts");
    TEST_ASSERT_FALSE(sar());
    TEST_ASSERT_EQUAL(GW_RNG_SRC_RADIO, gw_rng_note("the web certificate"));
    TEST_ASSERT_NOT_NULL(strstr(s_log, "I (rng) the commissioning password: made with the SAR ADC"));
    TEST_ASSERT_NOT_NULL(strstr(s_log, "I (rng) the web certificate: made with the radio"));
    TEST_ASSERT_NULL(strstr(s_log, "W (rng)"));
    TEST_ASSERT_NULL(strstr(s_log, "E (rng)"));
}

/* QEMU (no radio), or a radio that did not start: after gw_net_start no source with a guarantee
 * is on, and a value made then gives a warning. */
static void test_no_radio(void)
{
    gw_rng_boot();
    gw_rng_radio_starts();
    TEST_ASSERT_FALSE(sar());
    TEST_ASSERT_EQUAL(GW_RNG_SRC_NONE, gw_rng_note("the web certificate"));
    TEST_ASSERT_NOT_NULL(strstr(s_log, "W (rng) the web certificate: made with no entropy source"));
    gw_rng_radio_on(false);
    TEST_ASSERT_NOT_NULL(strstr(s_log, "E (rng) the radio did not start"));
    TEST_ASSERT_EQUAL(GW_RNG_SRC_NONE, gw_rng_source());
}

/* The SAR ADC source never comes on again after the radio or the sensor started: the source
 * must not be on with the RF subsystem or the ADC. */
static void test_no_sar_after_the_radio_or_the_sensor(void)
{
    gw_rng_boot();
    gw_rng_radio_starts();
    unsigned calls;
    gw_port_rng_sar_state(&calls);
    gw_rng_boot();
    unsigned after;
    TEST_ASSERT_FALSE(gw_port_rng_sar_state(&after));
    TEST_ASSERT_EQUAL_UINT(calls, after);
    TEST_ASSERT_NOT_NULL(strstr(s_log, "E (rng) SAR ADC entropy source not turned on"));

    gw_rng_reset();
    gw_rng_adc_starts("the temperature sensor starts");
    gw_rng_boot();
    TEST_ASSERT_FALSE(sar());
}

/* gw_net_start fails before the radio: the temperature sensor turns the source off. Each change
 * of the state calls the port one time; a call that changes nothing does not call it. */
static void test_the_sensor_turns_the_source_off(void)
{
    unsigned c0, c1, c2;
    gw_port_rng_sar_state(&c0);
    gw_rng_boot();
    gw_rng_boot();
    gw_port_rng_sar_state(&c1);
    TEST_ASSERT_EQUAL_UINT(c0 + 1u, c1);
    gw_rng_adc_starts("the temperature sensor starts");
    gw_rng_radio_starts();
    TEST_ASSERT_FALSE(gw_port_rng_sar_state(&c2));
    TEST_ASSERT_EQUAL_UINT(c1 + 1u, c2);
    TEST_ASSERT_NOT_NULL(strstr(s_log, "SAR ADC entropy source off: the temperature sensor starts"));
}

static void test_names(void)
{
    TEST_ASSERT_EQUAL_STRING("none", gw_rng_source_name(GW_RNG_SRC_NONE));
    TEST_ASSERT_EQUAL_STRING("SAR ADC", gw_rng_source_name(GW_RNG_SRC_SAR_ADC));
    TEST_ASSERT_EQUAL_STRING("radio", gw_rng_source_name(GW_RNG_SRC_RADIO));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_sequence_of_the_board);
    RUN_TEST(test_no_radio);
    RUN_TEST(test_no_sar_after_the_radio_or_the_sensor);
    RUN_TEST(test_the_sensor_turns_the_source_off);
    RUN_TEST(test_names);
    return UNITY_END();
}
