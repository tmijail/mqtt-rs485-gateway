/* QA-1: interface selection and failover policy (NET-4). */
#include "gw_netsel.h"
#include "unity.h"

enum { ETH = 0, WIFI = 1 };

static gw_netsel_t s;

void setUp(void) {}
void tearDown(void) {}

#define ASSERT_CHANGE(c, ev, f, t)             \
    do {                                       \
        gw_netsel_change_t c_ = (c);           \
        TEST_ASSERT_EQUAL_INT(ev, c_.event);   \
        TEST_ASSERT_EQUAL_INT(f, c_.from);     \
        TEST_ASSERT_EQUAL_INT(t, c_.to);       \
    } while (0)

#define UNCHANGED(c, a) ASSERT_CHANGE(c, GW_NETSEL_UNCHANGED, a, a)

/* Ethernet-only: up/down only, never a switch (the configuration of the MVP). */
static void test_ethernet_only(void)
{
    gw_netsel_init(&s, 1, (const uint8_t[]){0}, 30000);
    TEST_ASSERT_EQUAL_INT(GW_NETSEL_NONE, s.active);
    ASSERT_CHANGE(gw_netsel_set(&s, ETH, true, 100), GW_NETSEL_ACTIVATED, GW_NETSEL_NONE, ETH);
    UNCHANGED(gw_netsel_set(&s, ETH, true, 200), ETH);
    UNCHANGED(gw_netsel_poll(&s, 999999), ETH);
    ASSERT_CHANGE(gw_netsel_set(&s, ETH, false, 300), GW_NETSEL_LOST, ETH, GW_NETSEL_NONE);
    ASSERT_CHANGE(gw_netsel_set(&s, ETH, true, 400), GW_NETSEL_ACTIVATED, GW_NETSEL_NONE, ETH);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, gw_netsel_next_poll_ms(&s, 400));
    /* an index beyond the configured interfaces is ignored */
    UNCHANGED(gw_netsel_set(&s, WIFI, true, 500), ETH);
}

/* Wi-Fi as backup: Ethernet wins the default route; Wi-Fi only carries the session while
 * Ethernet is down, and Ethernet takes it back after failback_ms of stable link. */
static void test_wifi_backup_failover_and_failback(void)
{
    gw_netsel_init(&s, 2, (const uint8_t[]){0, 1}, 30000);
    /* Wi-Fi associates first at boot: used at once (availability first) */
    ASSERT_CHANGE(gw_netsel_set(&s, WIFI, true, 1000), GW_NETSEL_ACTIVATED, GW_NETSEL_NONE, WIFI);
    UNCHANGED(gw_netsel_set(&s, ETH, true, 2000), WIFI);
    TEST_ASSERT_EQUAL_UINT32(30000, gw_netsel_next_poll_ms(&s, 2000));
    TEST_ASSERT_EQUAL_UINT32(1, gw_netsel_next_poll_ms(&s, 31999));
    UNCHANGED(gw_netsel_poll(&s, 31999), WIFI);
    ASSERT_CHANGE(gw_netsel_poll(&s, 32000), GW_NETSEL_SWITCHED, WIFI, ETH);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, gw_netsel_next_poll_ms(&s, 32000));

    /* Ethernet cable pulled: immediate failover to Wi-Fi */
    ASSERT_CHANGE(gw_netsel_set(&s, ETH, false, 40000), GW_NETSEL_SWITCHED, ETH, WIFI);
    /* Wi-Fi going away while it is the backup in use: nothing left */
    ASSERT_CHANGE(gw_netsel_set(&s, WIFI, false, 41000), GW_NETSEL_LOST, WIFI, GW_NETSEL_NONE);
    /* the preferred interface comes back with nothing active: no failback wait */
    ASSERT_CHANGE(gw_netsel_set(&s, ETH, true, 42000), GW_NETSEL_ACTIVATED, GW_NETSEL_NONE, ETH);
    /* the backup coming and going while Ethernet is active changes nothing */
    UNCHANGED(gw_netsel_set(&s, WIFI, true, 43000), ETH);
    UNCHANGED(gw_netsel_poll(&s, 900000), ETH);
    UNCHANGED(gw_netsel_set(&s, WIFI, false, 900001), ETH);
}

/* Wi-Fi as primary: the same policy with the ranks swapped. */
static void test_wifi_primary(void)
{
    gw_netsel_init(&s, 2, (const uint8_t[]){1, 0}, 10000);
    ASSERT_CHANGE(gw_netsel_set(&s, ETH, true, 0), GW_NETSEL_ACTIVATED, GW_NETSEL_NONE, ETH);
    UNCHANGED(gw_netsel_set(&s, WIFI, true, 500), ETH);
    ASSERT_CHANGE(gw_netsel_poll(&s, 10500), GW_NETSEL_SWITCHED, ETH, WIFI);
    ASSERT_CHANGE(gw_netsel_set(&s, WIFI, false, 11000), GW_NETSEL_SWITCHED, WIFI, ETH);
}

/* A flapping preferred link restarts its stability timer on every flap: the session stays on
 * the backup until the link has been up for failback_ms in one stretch. */
static void test_flapping_preferred_link_does_not_steal_the_session(void)
{
    gw_netsel_init(&s, 2, (const uint8_t[]){0, 1}, 5000);
    gw_netsel_set(&s, WIFI, true, 0);
    for (uint64_t t = 1000; t < 20000; t += 2000) {
        UNCHANGED(gw_netsel_set(&s, ETH, true, t), WIFI);
        UNCHANGED(gw_netsel_poll(&s, t + 1000), WIFI);
        UNCHANGED(gw_netsel_set(&s, ETH, false, t + 1500), WIFI);
    }
    UNCHANGED(gw_netsel_set(&s, ETH, true, 30000), WIFI);
    /* a repeated "up" does not restart the timer */
    UNCHANGED(gw_netsel_set(&s, ETH, true, 33000), WIFI);
    ASSERT_CHANGE(gw_netsel_poll(&s, 35000), GW_NETSEL_SWITCHED, WIFI, ETH);
}

static void test_zero_failback_switches_at_once(void)
{
    gw_netsel_init(&s, 2, (const uint8_t[]){0, 1}, 0);
    gw_netsel_set(&s, WIFI, true, 0);
    ASSERT_CHANGE(gw_netsel_set(&s, ETH, true, 10), GW_NETSEL_SWITCHED, WIFI, ETH);
}

/* Issue #57: Ethernet keeps its link but the path to the broker is broken. The MQTT layer
 * reports it; Ethernet is held without service and Wi-Fi takes over at once. When the hold
 * ends, Ethernet wins the session back (the retry); reported again, the hold doubles; a
 * working session resets it. */
static void test_unreachable_broker_moves_the_session(void)
{
    gw_netsel_init(&s, 2, (const uint8_t[]){0, 1}, 30000);
    gw_netsel_set_hold(&s, 60000, 200000);
    gw_netsel_set(&s, ETH, true, 0);
    gw_netsel_set(&s, WIFI, true, 1000);
    UNCHANGED(gw_netsel_poll(&s, 100000), ETH);
    TEST_ASSERT_FALSE(gw_netsel_held(&s, ETH, 100000));

    ASSERT_CHANGE(gw_netsel_unreachable(&s, ETH, 100000), GW_NETSEL_SWITCHED, ETH, WIFI);
    TEST_ASSERT_TRUE(gw_netsel_held(&s, ETH, 100000));
    /* Ethernet still has link and address: it does not take the session back while held */
    UNCHANGED(gw_netsel_poll(&s, 159999), WIFI);
    TEST_ASSERT_EQUAL_UINT32(60000, gw_netsel_next_poll_ms(&s, 100000));
    /* the hold ends: failback to Ethernet (usable for far more than failback_ms) */
    ASSERT_CHANGE(gw_netsel_poll(&s, 160000), GW_NETSEL_SWITCHED, WIFI, ETH);
    /* still broken: held twice as long */
    ASSERT_CHANGE(gw_netsel_unreachable(&s, ETH, 170000), GW_NETSEL_SWITCHED, ETH, WIFI);
    UNCHANGED(gw_netsel_poll(&s, 289999), WIFI);
    ASSERT_CHANGE(gw_netsel_poll(&s, 290000), GW_NETSEL_SWITCHED, WIFI, ETH);
    /* and capped at the maximum */
    gw_netsel_unreachable(&s, ETH, 300000);
    UNCHANGED(gw_netsel_poll(&s, 499999), WIFI);
    ASSERT_CHANGE(gw_netsel_poll(&s, 500000), GW_NETSEL_SWITCHED, WIFI, ETH);
    gw_netsel_unreachable(&s, ETH, 510000);
    ASSERT_CHANGE(gw_netsel_poll(&s, 710000), GW_NETSEL_SWITCHED, WIFI, ETH);
    /* the path is back: a working session resets the hold to the minimum */
    gw_netsel_reachable(&s, ETH);
    TEST_ASSERT_FALSE(gw_netsel_held(&s, ETH, 710000));
    gw_netsel_unreachable(&s, ETH, 800000);
    ASSERT_CHANGE(gw_netsel_poll(&s, 860000), GW_NETSEL_SWITCHED, WIFI, ETH);
}

/* A held interface is still used when nothing else is usable; a cable pull or a held backup
 * never leaves the gateway without an interface. */
static void test_held_interface_is_the_last_resort(void)
{
    gw_netsel_init(&s, 2, (const uint8_t[]){0, 1}, 30000);
    gw_netsel_set(&s, ETH, true, 0);
    /* no Wi-Fi: the report changes nothing */
    UNCHANGED(gw_netsel_unreachable(&s, ETH, 1000), ETH);
    /* Wi-Fi comes up while Ethernet is held: it takes over at once, whatever its rank */
    ASSERT_CHANGE(gw_netsel_set(&s, WIFI, true, 2000), GW_NETSEL_SWITCHED, ETH, WIFI);
    /* Wi-Fi without service too: both held, stay where the session is */
    UNCHANGED(gw_netsel_unreachable(&s, WIFI, 3000), WIFI);
    /* Wi-Fi drops: back to the held Ethernet rather than nothing */
    ASSERT_CHANGE(gw_netsel_set(&s, WIFI, false, 4000), GW_NETSEL_SWITCHED, WIFI, ETH);
    /* Ethernet-only gateway (the MVP): reports never matter */
    gw_netsel_init(&s, 1, (const uint8_t[]){0}, 30000);
    gw_netsel_set(&s, ETH, true, 0);
    UNCHANGED(gw_netsel_unreachable(&s, ETH, 10), ETH);
    UNCHANGED(gw_netsel_poll(&s, 100), ETH);
    /* an index beyond the interfaces is ignored */
    UNCHANGED(gw_netsel_unreachable(&s, WIFI, 200), ETH);
    gw_netsel_reachable(&s, WIFI);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ethernet_only);
    RUN_TEST(test_wifi_backup_failover_and_failback);
    RUN_TEST(test_wifi_primary);
    RUN_TEST(test_flapping_preferred_link_does_not_steal_the_session);
    RUN_TEST(test_zero_failback_switches_at_once);
    RUN_TEST(test_unreachable_broker_moves_the_session);
    RUN_TEST(test_held_interface_is_the_last_resort);
    return UNITY_END();
}
