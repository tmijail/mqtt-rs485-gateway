/* NET-5, issue #187: the life of the access point (gw_aplife.h), with a fake clock: the stop on
 * request, the stop after N minutes without a client, the maximum time, and the end of the open
 * period at the claim. */
#include <string.h>

#include "gw_aplife.h"
#include "unity.h"

#define S(x) ((uint64_t)(x) * 1000u)
#define MIN(x) ((uint32_t)(x) * 60000u)

static gw_aplife_t l;

void setUp(void)
{
    gw_aplife_init(&l, MIN(5));
}

void tearDown(void) {}

/* Tick every second from @p from to @p to (inclusive); returns the first action and its time. */
static gw_aplife_action_t run(uint64_t from, uint64_t to, uint64_t *at, gw_ap_stop_t *why)
{
    for (uint64_t t = from; t <= to; t += 1000) {
        gw_aplife_action_t a = gw_aplife_tick(&l, t, why);
        if (a != GW_APLIFE_NONE) {
            *at = t;
            return a;
        }
    }
    *at = 0;
    *why = GW_AP_STOP_NONE;
    return GW_APLIFE_NONE;
}

static void test_not_running_does_nothing(void)
{
    gw_ap_stop_t why;
    uint32_t left;
    TEST_ASSERT_EQUAL(GW_APLIFE_NONE, gw_aplife_tick(&l, S(1000), &why));
    TEST_ASSERT_FALSE(gw_aplife_left_ms(&l, 0, &left));
    TEST_ASSERT_FALSE(gw_aplife_stop(&l, GW_AP_STOP_REQUEST));
    TEST_ASSERT_FALSE(gw_aplife_stop_soon(&l, 0, 1000));
    TEST_ASSERT_EQUAL(GW_AP_STOP_NONE, l.stopped);
    TEST_ASSERT_EQUAL_STRING("none", gw_ap_stop_key(l.stopped));
}

/* No client for N minutes: it stops, in every mode (no maximum time here). */
static void test_stops_after_the_idle_time(void)
{
    gw_aplife_start(&l, S(10), 0, 0);
    TEST_ASSERT_TRUE(l.running);
    TEST_ASSERT_FALSE(l.open);
    uint32_t left;
    TEST_ASSERT_TRUE(gw_aplife_left_ms(&l, S(10), &left));
    TEST_ASSERT_EQUAL_UINT32(MIN(5), left);
    uint64_t at;
    gw_ap_stop_t why;
    TEST_ASSERT_EQUAL(GW_APLIFE_STOP, run(S(10), S(10) + MIN(10), &at, &why));
    TEST_ASSERT_EQUAL(S(10) + MIN(5), at);
    TEST_ASSERT_EQUAL(GW_AP_STOP_IDLE, why);
    TEST_ASSERT_FALSE(l.running);
    TEST_ASSERT_EQUAL(GW_AP_STOP_IDLE, l.stopped);
    TEST_ASSERT_EQUAL_STRING("idle", gw_ap_stop_key(l.stopped));
}

/* A client keeps it on; the idle time counts again from the moment the last client leaves. */
static void test_a_client_keeps_it_on(void)
{
    gw_aplife_start(&l, 0, 0, 0);
    gw_aplife_clients(&l, 1, S(60));
    uint32_t left;
    TEST_ASSERT_FALSE(gw_aplife_left_ms(&l, S(61), &left));   /* no limit while connected */
    uint64_t at;
    gw_ap_stop_t why;
    TEST_ASSERT_EQUAL(GW_APLIFE_NONE, run(S(60), S(60) + MIN(60), &at, &why));
    gw_aplife_clients(&l, 2, S(4000));
    gw_aplife_clients(&l, 0, S(4000));
    TEST_ASSERT_TRUE(gw_aplife_left_ms(&l, S(4000), &left));
    TEST_ASSERT_EQUAL_UINT32(MIN(5), left);
    /* the same count again does not move the idle start */
    gw_aplife_clients(&l, 0, S(4100));
    TEST_ASSERT_TRUE(gw_aplife_left_ms(&l, S(4100), &left));
    TEST_ASSERT_EQUAL_UINT32(MIN(5) - 100000u, left);
    TEST_ASSERT_EQUAL(GW_APLIFE_STOP, run(S(4000), S(4000) + MIN(6), &at, &why));
    TEST_ASSERT_EQUAL(S(4000) + MIN(5), at);
    TEST_ASSERT_EQUAL(GW_AP_STOP_IDLE, why);
}

/* Normal mode: the maximum time stops it also with a client. */
static void test_maximum_time(void)
{
    gw_aplife_start(&l, 0, MIN(30), 0);
    gw_aplife_clients(&l, 1, S(1));
    uint32_t left;
    TEST_ASSERT_TRUE(gw_aplife_left_ms(&l, S(1), &left));
    TEST_ASSERT_EQUAL_UINT32(MIN(30) - 1000u, left);
    uint64_t at;
    gw_ap_stop_t why;
    TEST_ASSERT_EQUAL(GW_APLIFE_STOP, run(S(1), MIN(40), &at, &why));
    TEST_ASSERT_EQUAL(MIN(30), at);
    TEST_ASSERT_EQUAL(GW_AP_STOP_TIME, why);
    /* without a client the earlier of the two limits is the time left */
    gw_aplife_start(&l, 0, MIN(30), 0);
    TEST_ASSERT_TRUE(gw_aplife_left_ms(&l, 0, &left));
    TEST_ASSERT_EQUAL_UINT32(MIN(5), left);
    gw_aplife_start(&l, 0, MIN(3), 0);
    TEST_ASSERT_TRUE(gw_aplife_left_ms(&l, 0, &left));
    TEST_ASSERT_EQUAL_UINT32(MIN(3), left);
}

/* A new BOOT gesture starts the times again (the idle time and the maximum time). */
static void test_a_new_start_extends(void)
{
    gw_aplife_start(&l, 0, MIN(30), 0);
    gw_aplife_start(&l, MIN(4), MIN(30), 0);
    uint64_t at;
    gw_ap_stop_t why;
    TEST_ASSERT_EQUAL(GW_APLIFE_STOP, run(MIN(4), MIN(20), &at, &why));
    TEST_ASSERT_EQUAL(MIN(9), at);
    TEST_ASSERT_EQUAL_UINT32(1, l.starts);   /* the second start was an extension */
    gw_aplife_start(&l, MIN(21), 0, 0);
    TEST_ASSERT_EQUAL_UINT32(2, l.starts);
}

/* The stop on request: at once, or a short time later (the answer goes out first). */
static void test_stop_on_request(void)
{
    gw_aplife_start(&l, 0, 0, MIN(10));
    gw_aplife_clients(&l, 1, S(5));
    TEST_ASSERT_TRUE(gw_aplife_stop_soon(&l, S(20), 1000));
    uint32_t left;
    TEST_ASSERT_TRUE(gw_aplife_left_ms(&l, S(20), &left));
    TEST_ASSERT_EQUAL_UINT32(1000, left);
    gw_ap_stop_t why;
    TEST_ASSERT_EQUAL(GW_APLIFE_NONE, gw_aplife_tick(&l, S(20) + 999, &why));
    TEST_ASSERT_EQUAL(GW_APLIFE_STOP, gw_aplife_tick(&l, S(21), &why));
    TEST_ASSERT_EQUAL(GW_AP_STOP_REQUEST, why);
    TEST_ASSERT_FALSE(l.running);
    TEST_ASSERT_FALSE(l.open);
    TEST_ASSERT_EQUAL_UINT32(0, l.clients);
    /* a new start forgets an old request */
    gw_aplife_start(&l, S(30), 0, 0);
    TEST_ASSERT_EQUAL(GW_APLIFE_NONE, gw_aplife_tick(&l, S(40), &why));
    TEST_ASSERT_TRUE(gw_aplife_stop(&l, GW_AP_STOP_REQUEST));
    TEST_ASSERT_EQUAL(GW_AP_STOP_REQUEST, l.stopped);
    TEST_ASSERT_EQUAL_STRING("request", gw_ap_stop_key(l.stopped));
}

/* The open period of a claim window ends at the end of its time: WPA2 for the rest. */
static void test_open_period_ends_at_its_time(void)
{
    gw_aplife_start(&l, 0, MIN(30), MIN(10));
    gw_aplife_clients(&l, 1, S(1));
    TEST_ASSERT_TRUE(l.open);
    TEST_ASSERT_EQUAL_UINT32(MIN(10) - 1000u, gw_aplife_open_left_ms(&l, S(1)));
    uint64_t at;
    gw_ap_stop_t why;
    TEST_ASSERT_EQUAL(GW_APLIFE_SECURE, run(S(1), MIN(20), &at, &why));
    TEST_ASSERT_EQUAL(MIN(10), at);
    TEST_ASSERT_FALSE(l.open);
    TEST_ASSERT_TRUE(l.running);
    TEST_ASSERT_EQUAL_UINT32(0, gw_aplife_open_left_ms(&l, MIN(10)));
    /* a start without an open period while open: WPA2 at once (a second BOOT gesture after the
     * claim, with the password set) */
    gw_aplife_start(&l, MIN(11), MIN(30), MIN(10));
    TEST_ASSERT_TRUE(l.open);
    gw_aplife_start(&l, MIN(12), MIN(30), 0);
    TEST_ASSERT_FALSE(l.open);
}

/* The claim ends the open period a few seconds after its answer. */
static void test_claim_ends_the_open_period(void)
{
    gw_aplife_start(&l, 0, 0, MIN(10));
    gw_aplife_clients(&l, 1, S(30));
    gw_aplife_claimed(&l, S(90));
    TEST_ASSERT_TRUE(l.open);
    TEST_ASSERT_EQUAL_UINT32(GW_APLIFE_CLAIM_SECURE_MS, gw_aplife_open_left_ms(&l, S(90)));
    uint64_t at;
    gw_ap_stop_t why;
    TEST_ASSERT_EQUAL(GW_APLIFE_SECURE, run(S(90), MIN(10), &at, &why));
    TEST_ASSERT_EQUAL(S(90) + GW_APLIFE_CLAIM_SECURE_MS, at);
    TEST_ASSERT_TRUE(l.running);
    /* the disconnection of the operator: the idle time starts again from it */
    gw_aplife_clients(&l, 0, at);
    TEST_ASSERT_EQUAL(GW_APLIFE_STOP, run(at, at + MIN(6), &at, &why));
    TEST_ASSERT_EQUAL(GW_AP_STOP_IDLE, why);
}

/* A claim near the end of the open period does not make it longer; a claim without an open
 * period (WPA2) changes nothing. */
static void test_claim_never_extends(void)
{
    gw_aplife_start(&l, 0, 0, 3000);
    gw_aplife_claimed(&l, 1000);
    TEST_ASSERT_EQUAL_UINT32(2000, gw_aplife_open_left_ms(&l, 1000));
    gw_aplife_start(&l, S(10), 0, 0);
    gw_aplife_claimed(&l, S(11));
    TEST_ASSERT_FALSE(l.open);
    gw_ap_stop_t why;
    TEST_ASSERT_EQUAL(GW_APLIFE_NONE, gw_aplife_tick(&l, S(20), &why));
}

/* A stop has priority over the end of the open period at the same tick; idle 0 = never. */
static void test_stop_has_priority_and_idle_zero(void)
{
    gw_aplife_start(&l, 0, MIN(5), MIN(5));
    gw_ap_stop_t why;
    TEST_ASSERT_EQUAL(GW_APLIFE_STOP, gw_aplife_tick(&l, MIN(5), &why));
    TEST_ASSERT_EQUAL(GW_AP_STOP_TIME, why);
    gw_aplife_init(&l, 0);
    gw_aplife_start(&l, 0, 0, 0);
    uint32_t left;
    TEST_ASSERT_FALSE(gw_aplife_left_ms(&l, MIN(100), &left));
    TEST_ASSERT_EQUAL(GW_APLIFE_NONE, gw_aplife_tick(&l, MIN(1000), &why));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_not_running_does_nothing);
    RUN_TEST(test_stops_after_the_idle_time);
    RUN_TEST(test_a_client_keeps_it_on);
    RUN_TEST(test_maximum_time);
    RUN_TEST(test_a_new_start_extends);
    RUN_TEST(test_stop_on_request);
    RUN_TEST(test_open_period_ends_at_its_time);
    RUN_TEST(test_claim_ends_the_open_period);
    RUN_TEST(test_claim_never_extends);
    RUN_TEST(test_stop_has_priority_and_idle_zero);
    return UNITY_END();
}
