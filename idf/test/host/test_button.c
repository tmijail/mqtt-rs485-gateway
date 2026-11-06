/* QA-1: BOOT button gestures (CFG-5 factory reset, NET-5 access point). */
#include "gw_button.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* Feed a constant level from @p *t for @p ms in 10 ms samples; returns the first event. */
static gw_btn_event_t hold(gw_button_t *b, bool pressed, uint32_t *t, uint32_t ms)
{
    gw_btn_event_t first = GW_BTN_NONE;
    for (uint32_t n = 0; n < ms; n += 10, *t += 10) {   /* wraps like the real clock */
        gw_btn_event_t e = gw_button_update(b, pressed, *t);
        if (first == GW_BTN_NONE) {
            first = e;
        }
    }
    return first;
}

static void test_factory_reset_after_10_s(void)
{
    gw_button_t b;
    uint32_t t = 1000;
    gw_button_init(&b, t);
    TEST_ASSERT_EQUAL(GW_BTN_NONE, hold(&b, false, &t, 200));
    TEST_ASSERT_EQUAL(GW_BTN_NONE, hold(&b, true, &t, 9990));   /* 9.94 s debounced */
    TEST_ASSERT_TRUE(gw_button_held_ms(&b, t) > 9000);
    TEST_ASSERT_EQUAL(GW_BTN_FACTORY_RESET, hold(&b, true, &t, 200));
    TEST_ASSERT_EQUAL(GW_BTN_NONE, hold(&b, true, &t, 20000));  /* once per press */
    TEST_ASSERT_EQUAL(GW_BTN_NONE, hold(&b, false, &t, 200));   /* no access point after it */
}

static void test_commissioning_press(void)
{
    gw_button_t b;
    uint32_t t = 0;
    gw_button_init(&b, t);
    hold(&b, false, &t, 100);
    TEST_ASSERT_EQUAL(GW_BTN_NONE, hold(&b, true, &t, 1500));
    TEST_ASSERT_EQUAL(GW_BTN_NONE, hold(&b, false, &t, 200));   /* 1.5 s: too short */
    TEST_ASSERT_EQUAL(GW_BTN_NONE, hold(&b, true, &t, 3000));
    TEST_ASSERT_EQUAL(GW_BTN_COMMISSIONING, hold(&b, false, &t, 200));
    TEST_ASSERT_EQUAL(0, gw_button_held_ms(&b, t));
}

static void test_held_since_boot_and_bounces(void)
{
    gw_button_t b;
    uint32_t t = 0;
    gw_button_init(&b, t);
    /* pressed from the start (or stuck): never a reset, however long */
    TEST_ASSERT_EQUAL(GW_BTN_NONE, hold(&b, true, &t, 30000));
    TEST_ASSERT_EQUAL(GW_BTN_NONE, hold(&b, false, &t, 100));   /* first release only arms */
    /* bounces shorter than the debounce do not end a press */
    TEST_ASSERT_EQUAL(GW_BTN_NONE, hold(&b, true, &t, 5000));
    TEST_ASSERT_EQUAL(GW_BTN_NONE, hold(&b, false, &t, 30));
    TEST_ASSERT_EQUAL(GW_BTN_FACTORY_RESET, hold(&b, true, &t, 5100));
    /* a 30 ms blip is no press at all */
    hold(&b, false, &t, 200);
    TEST_ASSERT_EQUAL(GW_BTN_NONE, hold(&b, true, &t, 30));
    TEST_ASSERT_EQUAL(GW_BTN_NONE, hold(&b, false, &t, 200));
    TEST_ASSERT_EQUAL(0, gw_button_held_ms(&b, t));
}

static void test_clock_wraps(void)
{
    gw_button_t b;
    uint32_t t = UINT32_MAX - 3000;
    gw_button_init(&b, t);
    hold(&b, false, &t, 100);
    TEST_ASSERT_EQUAL(GW_BTN_FACTORY_RESET, hold(&b, true, &t, 10200));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_factory_reset_after_10_s);
    RUN_TEST(test_commissioning_press);
    RUN_TEST(test_held_since_boot_and_bounces);
    RUN_TEST(test_clock_wraps);
    return UNITY_END();
}
