/* QA-1: buzzer patterns and alarms (IO-6). */
#include <string.h>

#include "gw_buzz.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* Play from @p from to @p to (ms, step 10) and count the beeps (rising edges of the tone);
 * *first gets the time of the first one (or UINT32_MAX). */
static int beeps(gw_buzz_t *b, uint32_t from, uint32_t to, uint32_t *first)
{
    int n = 0;
    bool prev = b->tone;
    if (first) {
        *first = UINT32_MAX;
    }
    for (uint32_t t = from; t < to; t += 10) {
        bool on = gw_buzz_tone(b, t);
        if (on && !prev) {
            n++;
            if (first && *first == UINT32_MAX) {
                *first = t;
            }
        }
        prev = on;
    }
    return n;
}

static void test_request_plays_n_beeps(void)
{
    gw_buzz_t b;
    gw_buzz_init(&b, 0, 0);
    TEST_ASSERT_TRUE(gw_buzz_idle(&b));
    gw_buzz_request(&b, 2);   /* CFG-5 factory reset confirmation */
    TEST_ASSERT_FALSE(gw_buzz_idle(&b));
    uint32_t first;
    TEST_ASSERT_EQUAL(2, beeps(&b, 1000, 1600, &first));
    TEST_ASSERT_EQUAL_UINT32(1000, first);
    TEST_ASSERT_TRUE(gw_buzz_idle(&b));
    /* tone lasts GW_BUZZ_ON_MS */
    gw_buzz_request(&b, 1);
    TEST_ASSERT_TRUE(gw_buzz_tone(&b, 2000));
    TEST_ASSERT_TRUE(gw_buzz_tone(&b, 2140));
    TEST_ASSERT_FALSE(gw_buzz_tone(&b, 2150));
    TEST_ASSERT_EQUAL(0, beeps(&b, 2160, 20000, NULL));   /* nothing else */
}

static void test_alarms_priority_delay_and_period(void)
{
    gw_buzz_t b;
    gw_buzz_init(&b, GW_ALARM_BIT(GW_ALARM_MQTT_DOWN) | GW_ALARM_BIT(GW_ALARM_DEVICE_OFFLINE), 30);
    /* conditions present from t = 0: nothing before 30 s */
    TEST_ASSERT_EQUAL(GW_ALARM_NONE, gw_buzz_update(&b, false, true, true, 0));
    TEST_ASSERT_EQUAL(GW_ALARM_NONE, gw_buzz_update(&b, false, true, true, 29999));
    TEST_ASSERT_EQUAL(0, beeps(&b, 0, 29990, NULL));
    TEST_ASSERT_EQUAL(GW_ALARM_MQTT_DOWN, gw_buzz_update(&b, false, true, true, 30000));
    uint32_t first;
    TEST_ASSERT_EQUAL(2, beeps(&b, 30000, 39990, &first));      /* at once, 2 beeps */
    TEST_ASSERT_EQUAL_UINT32(30000, first);
    TEST_ASSERT_EQUAL(2, beeps(&b, 39990, 49990, &first));      /* again 10 s later */
    TEST_ASSERT_EQUAL_UINT32(40000, first);
    /* the session comes back: the lower alarm takes over at once */
    TEST_ASSERT_EQUAL(GW_ALARM_DEVICE_OFFLINE, gw_buzz_update(&b, false, false, true, 50000));
    TEST_ASSERT_EQUAL(1, beeps(&b, 50000, 59990, &first));
    TEST_ASSERT_EQUAL_UINT32(50000, first);
    TEST_ASSERT_EQUAL(GW_ALARM_NONE, gw_buzz_update(&b, false, false, false, 60000));
    TEST_ASSERT_EQUAL(0, beeps(&b, 60000, 90000, NULL));
    /* a short outage (< 30 s) never sounds */
    gw_buzz_update(&b, false, true, false, 100000);
    TEST_ASSERT_EQUAL(GW_ALARM_NONE, gw_buzz_update(&b, false, false, false, 129000));
    /* configuration error: not enabled here, so silent */
    TEST_ASSERT_EQUAL(GW_ALARM_NONE, gw_buzz_update(&b, true, false, false, 130000));
}

static void test_config_error_at_once_and_requests_first(void)
{
    gw_buzz_t b;
    gw_buzz_init(&b, GW_ALARM_BIT(GW_ALARM_CONFIG_ERROR) | GW_ALARM_BIT(GW_ALARM_MQTT_DOWN), 30);
    TEST_ASSERT_EQUAL(GW_ALARM_CONFIG_ERROR, gw_buzz_update(&b, true, true, false, 5));
    gw_buzz_request(&b, 2);   /* a request queued with the alarm: the request goes first */
    TEST_ASSERT_EQUAL(2, beeps(&b, 10, 610, NULL));
    TEST_ASSERT_EQUAL(3, beeps(&b, 610, 9990, NULL));
    TEST_ASSERT_EQUAL_STRING("configuration error", gw_alarm_name(GW_ALARM_CONFIG_ERROR));
    TEST_ASSERT_EQUAL_STRING("MQTT down", gw_alarm_name(GW_ALARM_MQTT_DOWN));
    /* a silent buzzer (no alarm enabled) only plays requests */
    gw_buzz_init(&b, 0, 0);
    TEST_ASSERT_EQUAL(GW_ALARM_NONE, gw_buzz_update(&b, true, true, true, 0));
    TEST_ASSERT_EQUAL(0, beeps(&b, 0, 30000, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_request_plays_n_beeps);
    RUN_TEST(test_alarms_priority_delay_and_period);
    RUN_TEST(test_config_error_at_once_and_requests_first);
    return UNITY_END();
}
