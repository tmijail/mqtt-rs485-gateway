/* QA-1: BOOT button gestures (CFG-5 factory reset, NET-5 access point). Issue #292: a mark at
 * 2 s and at 10 s while the button is held; the action only at the release. */
#include "gw_button.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define SAMPLE_MS 10
#define MAX_EV    8

typedef struct {
    int n;
    gw_btn_event_t ev[MAX_EV];
    uint32_t at[MAX_EV];   /* time of each event */
} events_t;

/* Feed a constant level from @p *t for @p ms in 10 ms samples; records each event in @p out
 * (may be NULL) and returns the first one. */
static gw_btn_event_t feed(gw_button_t *b, bool pressed, uint32_t *t, uint32_t ms, events_t *out)
{
    gw_btn_event_t first = GW_BTN_NONE;
    for (uint32_t n = 0; n < ms; n += SAMPLE_MS, *t += SAMPLE_MS) {   /* wraps like the clock */
        gw_btn_event_t e = gw_button_update(b, pressed, *t);
        if (e == GW_BTN_NONE) {
            continue;
        }
        if (first == GW_BTN_NONE) {
            first = e;
        }
        if (out != NULL && out->n < MAX_EV) {
            out->ev[out->n] = e;
            out->at[out->n] = *t;
            out->n++;
        }
    }
    return first;
}

static gw_btn_event_t hold(gw_button_t *b, bool pressed, uint32_t *t, uint32_t ms)
{
    return feed(b, pressed, t, ms, NULL);
}

/* An armed button at @p *t: seen released once. */
static void armed(gw_button_t *b, uint32_t *t)
{
    gw_button_init(b, *t);
    TEST_ASSERT_EQUAL(GW_BTN_NONE, hold(b, false, t, 200));
}

/* Issue #292: 2 s -> MARK_AP, 10 s -> MARK_RESET, while held; the factory reset only at the
 * release, however long after the 10 s mark. */
static void test_marks_then_factory_reset_at_release(void)
{
    gw_button_t b;
    uint32_t t = 1000;
    armed(&b, &t);
    uint32_t down = t;
    events_t ev = {0};
    feed(&b, true, &t, 30000, &ev);
    TEST_ASSERT_EQUAL(2, ev.n);
    TEST_ASSERT_EQUAL(GW_BTN_MARK_AP, ev.ev[0]);
    TEST_ASSERT_EQUAL(GW_BTN_MARK_RESET, ev.ev[1]);
    /* the marks come at the limits, plus the debounce and one sample at most */
    uint32_t late = GW_BTN_DEBOUNCE_MS + SAMPLE_MS;
    TEST_ASSERT_UINT32_WITHIN(late, GW_BTN_AP_MS + late / 2, ev.at[0] - down);
    TEST_ASSERT_UINT32_WITHIN(late, GW_BTN_RESET_MS + late / 2, ev.at[1] - down);
    TEST_ASSERT_EQUAL(GW_BTN_PHASE_RESET, gw_button_phase(&b));
    TEST_ASSERT_TRUE(gw_button_held_ms(&b, t) > 29000);
    /* the release: the factory reset, after the debounce */
    uint32_t up = t;
    events_t rel = {0};
    feed(&b, false, &t, 200, &rel);
    TEST_ASSERT_EQUAL(1, rel.n);
    TEST_ASSERT_EQUAL(GW_BTN_FACTORY_RESET, rel.ev[0]);
    TEST_ASSERT_UINT32_WITHIN(SAMPLE_MS, GW_BTN_DEBOUNCE_MS, rel.at[0] - up);
    TEST_ASSERT_EQUAL(GW_BTN_PHASE_NONE, gw_button_phase(&b));
    TEST_ASSERT_EQUAL(0, gw_button_held_ms(&b, t));
}

/* Just before 10 s: no factory reset yet (no mark); a release at 9.9 s is the access point. */
static void test_no_reset_before_10_s(void)
{
    gw_button_t b;
    uint32_t t = 0;
    armed(&b, &t);
    TEST_ASSERT_EQUAL(GW_BTN_MARK_AP, hold(&b, true, &t, 9900));
    TEST_ASSERT_EQUAL(GW_BTN_PHASE_AP, gw_button_phase(&b));
    TEST_ASSERT_EQUAL(GW_BTN_COMMISSIONING, hold(&b, false, &t, 200));
}

/* A release at 9.97 s: the raw sample is up when the 10 s mark would come (the debounce still
 * reports the press): no MARK_RESET, and the release is the access point, not a reset. */
static void test_release_during_debounce_at_10_s(void)
{
    gw_button_t b;
    uint32_t t = 0;
    armed(&b, &t);
    events_t ev = {0};
    feed(&b, true, &t, GW_BTN_DEBOUNCE_MS + GW_BTN_RESET_MS - 30, &ev);   /* 9.97 s debounced */
    feed(&b, false, &t, 200, &ev);
    TEST_ASSERT_EQUAL(2, ev.n);
    TEST_ASSERT_EQUAL(GW_BTN_MARK_AP, ev.ev[0]);
    TEST_ASSERT_EQUAL(GW_BTN_COMMISSIONING, ev.ev[1]);
}

static void test_commissioning_press(void)
{
    gw_button_t b;
    uint32_t t = 0;
    armed(&b, &t);
    TEST_ASSERT_EQUAL(GW_BTN_NONE, hold(&b, true, &t, 1500));
    TEST_ASSERT_EQUAL(GW_BTN_NONE, hold(&b, false, &t, 200));   /* 1.5 s: too short, no mark */
    uint32_t down = t;
    events_t ev = {0};
    feed(&b, true, &t, 3000, &ev);
    TEST_ASSERT_EQUAL(1, ev.n);
    TEST_ASSERT_EQUAL(GW_BTN_MARK_AP, ev.ev[0]);   /* the 1 beep at 2 s, before the release */
    TEST_ASSERT_TRUE(ev.at[0] - down >= GW_BTN_AP_MS);
    TEST_ASSERT_TRUE(ev.at[0] - down < GW_BTN_AP_MS + GW_BTN_DEBOUNCE_MS + 2 * SAMPLE_MS);
    TEST_ASSERT_EQUAL(GW_BTN_COMMISSIONING, hold(&b, false, &t, 200));
    TEST_ASSERT_EQUAL(0, gw_button_held_ms(&b, t));
    /* the next press starts its marks again */
    TEST_ASSERT_EQUAL(GW_BTN_MARK_AP, hold(&b, true, &t, 2500));
    TEST_ASSERT_EQUAL(GW_BTN_COMMISSIONING, hold(&b, false, &t, 200));
}

/* Held or stuck since power-up: no mark and no gesture, however long; the first release only
 * arms the button. */
static void test_held_since_boot(void)
{
    gw_button_t b;
    uint32_t t = 0;
    gw_button_init(&b, t);
    events_t ev = {0};
    feed(&b, true, &t, 60000, &ev);
    TEST_ASSERT_EQUAL(0, ev.n);
    TEST_ASSERT_EQUAL(GW_BTN_PHASE_NONE, gw_button_phase(&b));
    TEST_ASSERT_EQUAL(GW_BTN_NONE, hold(&b, false, &t, 100));   /* first release only arms */
    TEST_ASSERT_EQUAL(GW_BTN_MARK_AP, hold(&b, true, &t, 2100));
}

/* Issue #292: a button that sticks down after it was armed (or a person who holds it for
 * minutes): the two marks, then nothing while it stays down. No factory reset without the
 * release, and no cancel time: a release, also much later, starts the factory reset. */
static void test_stuck_after_armed(void)
{
    gw_button_t b;
    uint32_t t = 0;
    armed(&b, &t);
    events_t ev = {0};
    feed(&b, true, &t, 30u * 60u * 1000u, &ev);   /* 30 min */
    TEST_ASSERT_EQUAL(2, ev.n);
    TEST_ASSERT_EQUAL(GW_BTN_MARK_AP, ev.ev[0]);
    TEST_ASSERT_EQUAL(GW_BTN_MARK_RESET, ev.ev[1]);
    TEST_ASSERT_EQUAL(GW_BTN_PHASE_RESET, gw_button_phase(&b));
    TEST_ASSERT_EQUAL(GW_BTN_FACTORY_RESET, hold(&b, false, &t, 200));
}

/* Bounces shorter than the debounce do not end a press; a mark waits for the raw sample to be
 * down again; a 30 ms blip is no press. */
static void test_bounces(void)
{
    gw_button_t b;
    uint32_t t = 0;
    armed(&b, &t);
    TEST_ASSERT_EQUAL(GW_BTN_MARK_AP, hold(&b, true, &t, 5000));
    TEST_ASSERT_EQUAL(GW_BTN_NONE, hold(&b, false, &t, 30));
    TEST_ASSERT_EQUAL(GW_BTN_MARK_RESET, hold(&b, true, &t, 5100));
    TEST_ASSERT_EQUAL(GW_BTN_FACTORY_RESET, hold(&b, false, &t, 200));
    /* a bounce up at the 2 s limit: the mark comes when the contact is closed again */
    uint32_t down = t;
    events_t ev = {0};
    feed(&b, true, &t, 2040, &ev);   /* debounced at +50 ms: held 1980 ms at the last sample */
    TEST_ASSERT_EQUAL(0, ev.n);
    feed(&b, false, &t, 30, &ev);    /* the limit (held 2000 ms) comes while the contact is open */
    TEST_ASSERT_EQUAL(0, ev.n);
    TEST_ASSERT_EQUAL(GW_BTN_PHASE_NONE, gw_button_phase(&b));
    feed(&b, true, &t, 100, &ev);
    TEST_ASSERT_EQUAL(1, ev.n);
    TEST_ASSERT_EQUAL(GW_BTN_MARK_AP, ev.ev[0]);
    TEST_ASSERT_EQUAL(2070, ev.at[0] - down);   /* the first sample with the contact closed */
    TEST_ASSERT_EQUAL(GW_BTN_COMMISSIONING, hold(&b, false, &t, 200));
    /* a 30 ms blip is no press at all */
    TEST_ASSERT_EQUAL(GW_BTN_NONE, hold(&b, true, &t, 30));
    TEST_ASSERT_EQUAL(GW_BTN_NONE, hold(&b, false, &t, 200));
    TEST_ASSERT_EQUAL(0, gw_button_held_ms(&b, t));
}

static void test_clock_wraps(void)
{
    gw_button_t b;
    uint32_t t = UINT32_MAX - 3000;
    armed(&b, &t);
    events_t ev = {0};
    feed(&b, true, &t, 10200, &ev);
    feed(&b, false, &t, 200, &ev);
    TEST_ASSERT_EQUAL(3, ev.n);
    TEST_ASSERT_EQUAL(GW_BTN_MARK_AP, ev.ev[0]);
    TEST_ASSERT_EQUAL(GW_BTN_MARK_RESET, ev.ev[1]);
    TEST_ASSERT_EQUAL(GW_BTN_FACTORY_RESET, ev.ev[2]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_marks_then_factory_reset_at_release);
    RUN_TEST(test_no_reset_before_10_s);
    RUN_TEST(test_release_during_debounce_at_10_s);
    RUN_TEST(test_commissioning_press);
    RUN_TEST(test_held_since_boot);
    RUN_TEST(test_stuck_after_armed);
    RUN_TEST(test_bounces);
    RUN_TEST(test_clock_wraps);
    return UNITY_END();
}
