/* Issue #208: the owners of the Wi-Fi driver (gw_wifiown.h). A scan with the radio off starts
 * the driver and gives it back as at the boot, so that a second scan and the commissioning access
 * point start it again; a scan with the station or the access point only adds or removes the
 * interface of the station. */
#include "gw_wifiown.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* A model of the driver: the plans applied in sequence, with the checks of esp_wifi (set_mode
 * and start need init; deinit needs stop). */
typedef struct {
    bool inited, started;
    gw_wifi_mode_t mode;
    int inits, deinits;
} drv_t;

static void apply(drv_t *d, gw_wifiown_plan_t p)
{
    if (p.init) {
        TEST_ASSERT_FALSE(d->inited);
        d->inited = true;
        d->mode = GW_WIFI_MODE_NULL;
        d->inits++;
    }
    if (p.set_mode) {
        TEST_ASSERT_TRUE(d->inited);
        d->mode = p.mode;
    }
    if (p.start) {
        TEST_ASSERT_TRUE(d->inited);
        TEST_ASSERT_FALSE(d->started);
        d->started = true;
    }
    if (p.stop) {
        TEST_ASSERT_TRUE(d->started);
        d->started = false;
    }
    if (p.deinit) {
        TEST_ASSERT_TRUE(d->inited);
        TEST_ASSERT_FALSE(d->started);
        d->inited = false;
        d->mode = GW_WIFI_MODE_NULL;
        d->deinits++;
    }
}

static uint8_t go(drv_t *d, uint8_t users, uint8_t after)
{
    apply(d, gw_wifiown_plan(users, after));
    return after;
}

static void test_modes(void)
{
    TEST_ASSERT_EQUAL(GW_WIFI_MODE_NULL, gw_wifiown_mode(0));
    TEST_ASSERT_EQUAL(GW_WIFI_MODE_STA, gw_wifiown_mode(GW_WIFI_USER_STA));
    TEST_ASSERT_EQUAL(GW_WIFI_MODE_STA, gw_wifiown_mode(GW_WIFI_USER_SCAN));
    TEST_ASSERT_EQUAL(GW_WIFI_MODE_STA, gw_wifiown_mode(GW_WIFI_USER_STA | GW_WIFI_USER_SCAN));
    TEST_ASSERT_EQUAL(GW_WIFI_MODE_AP, gw_wifiown_mode(GW_WIFI_USER_AP));
    TEST_ASSERT_EQUAL(GW_WIFI_MODE_APSTA, gw_wifiown_mode(GW_WIFI_USER_AP | GW_WIFI_USER_SCAN));
    TEST_ASSERT_EQUAL(GW_WIFI_MODE_APSTA, gw_wifiown_mode(GW_WIFI_USER_AP | GW_WIFI_USER_STA));
    TEST_ASSERT_EQUAL(GW_WIFI_MODE_STA, gw_wifiown_mode(GW_WIFI_USER_RNG));
    TEST_ASSERT_EQUAL(GW_WIFI_MODE_APSTA, gw_wifiown_mode(GW_WIFI_USER_AP | GW_WIFI_USER_RNG));
}

/* The finding of the review: after a scan with the radio off, the driver stayed in STA mode,
 * stopped and initialised. A second scan failed ("not started"), the access point did not start,
 * and the RAM of the driver stayed allocated. */
static void test_scan_with_the_radio_off_gives_the_driver_back(void)
{
    drv_t d = {0};
    uint8_t u = 0;
    for (int scan = 0; scan < 2; scan++) {
        u = go(&d, u, u | GW_WIFI_USER_SCAN);
        TEST_ASSERT_TRUE(d.inited && d.started);
        TEST_ASSERT_EQUAL(GW_WIFI_MODE_STA, d.mode);
        u = go(&d, u, (uint8_t)(u & ~GW_WIFI_USER_SCAN));
        TEST_ASSERT_FALSE(d.inited);
        TEST_ASSERT_FALSE(d.started);
    }
    TEST_ASSERT_EQUAL(2, d.inits);
    TEST_ASSERT_EQUAL(2, d.deinits);
    /* then the access point: the driver starts in AP mode */
    u = go(&d, u, GW_WIFI_USER_AP);
    TEST_ASSERT_TRUE(d.inited && d.started);
    TEST_ASSERT_EQUAL(GW_WIFI_MODE_AP, d.mode);
    u = go(&d, u, 0);
    TEST_ASSERT_FALSE(d.inited);
    TEST_ASSERT_EQUAL(0, u);
}

/* A scan while the access point runs adds the station interface, and removes it at the end:
 * the driver does not stop, the clients of the access point stay. */
static void test_scan_during_the_access_point(void)
{
    drv_t d = {0};
    uint8_t u = go(&d, 0, GW_WIFI_USER_AP);
    gw_wifiown_plan_t p = gw_wifiown_plan(u, u | GW_WIFI_USER_SCAN);
    TEST_ASSERT_FALSE(p.init || p.start || p.stop || p.deinit);
    TEST_ASSERT_TRUE(p.set_mode);
    TEST_ASSERT_EQUAL(GW_WIFI_MODE_APSTA, p.mode);
    apply(&d, p);
    u |= GW_WIFI_USER_SCAN;
    p = gw_wifiown_plan(u, GW_WIFI_USER_AP);
    TEST_ASSERT_FALSE(p.init || p.start || p.stop || p.deinit);
    TEST_ASSERT_EQUAL(GW_WIFI_MODE_AP, p.mode);
    apply(&d, p);
    TEST_ASSERT_TRUE(d.started);
    TEST_ASSERT_EQUAL(1, d.inits);
}

/* The access point stops during a scan that started it... and the reverse: the driver stays
 * until the last user goes, in the mode of the users that stay. */
static void test_overlap_of_the_access_point_and_a_scan(void)
{
    drv_t d = {0};
    uint8_t u = go(&d, 0, GW_WIFI_USER_SCAN);
    u = go(&d, u, u | GW_WIFI_USER_AP);   /* the access point starts during the scan */
    TEST_ASSERT_EQUAL(GW_WIFI_MODE_APSTA, d.mode);
    TEST_ASSERT_EQUAL(1, d.inits);
    u = go(&d, u, GW_WIFI_USER_AP);       /* the scan ends */
    TEST_ASSERT_EQUAL(GW_WIFI_MODE_AP, d.mode);
    TEST_ASSERT_TRUE(d.started);
    u = go(&d, u, u | GW_WIFI_USER_SCAN); /* a new scan */
    u = go(&d, u, GW_WIFI_USER_SCAN);     /* the access point stops during it */
    TEST_ASSERT_EQUAL(GW_WIFI_MODE_STA, d.mode);
    TEST_ASSERT_TRUE(d.started);
    go(&d, u, 0);                         /* the scan ends: nothing is left */
    TEST_ASSERT_FALSE(d.inited);
    TEST_ASSERT_EQUAL(1, d.deinits);
}

/* With the station of network.wifi the driver never stops: a scan and the access point only
 * change the mode. */
static void test_station_keeps_the_driver(void)
{
    drv_t d = {0};
    uint8_t u = go(&d, 0, GW_WIFI_USER_STA);
    TEST_ASSERT_EQUAL(GW_WIFI_MODE_STA, d.mode);
    gw_wifiown_plan_t p = gw_wifiown_plan(u, u | GW_WIFI_USER_SCAN);
    TEST_ASSERT_FALSE(p.init || p.set_mode || p.start || p.stop || p.deinit);
    p = gw_wifiown_plan(u | GW_WIFI_USER_SCAN, u);
    TEST_ASSERT_FALSE(p.init || p.set_mode || p.start || p.stop || p.deinit);
    u = go(&d, u, u | GW_WIFI_USER_AP);
    TEST_ASSERT_EQUAL(GW_WIFI_MODE_APSTA, d.mode);
    go(&d, u, GW_WIFI_USER_STA);
    TEST_ASSERT_EQUAL(GW_WIFI_MODE_STA, d.mode);
    TEST_ASSERT_TRUE(d.started);
    TEST_ASSERT_EQUAL(1, d.inits);
    TEST_ASSERT_EQUAL(0, d.deinits);
}

/* Issue #267: the radio of the entropy source (GW_WIFI_USER_RNG) keeps the driver from the boot
 * until the restart. The station comes first (gw_net_start); the scan and the access point only
 * add and remove their interface; when they go, the driver does not stop. */
static void test_the_radio_of_the_rng_never_stops(void)
{
    drv_t d = {0};
    /* with the station of network.wifi: the station starts the driver, the RNG changes nothing */
    uint8_t u = go(&d, 0, GW_WIFI_USER_STA);
    gw_wifiown_plan_t p = gw_wifiown_plan(u, u | GW_WIFI_USER_RNG);
    TEST_ASSERT_FALSE(p.init || p.set_mode || p.start || p.stop || p.deinit || p.refuse);
    TEST_ASSERT_FALSE(p.sta_start);
    u |= GW_WIFI_USER_RNG;
    u = go(&d, u, u | GW_WIFI_USER_AP);
    go(&d, u, (uint8_t)(u & ~GW_WIFI_USER_AP));
    TEST_ASSERT_TRUE(d.started);
    TEST_ASSERT_EQUAL(GW_WIFI_MODE_STA, d.mode);

    /* with no station: the RNG starts the driver in STA mode */
    drv_t e = {0};
    p = gw_wifiown_plan(0, GW_WIFI_USER_RNG);
    TEST_ASSERT_TRUE(p.init && p.set_mode && p.start && p.sta_start);
    TEST_ASSERT_EQUAL(GW_WIFI_MODE_STA, p.mode);
    apply(&e, p);
    u = GW_WIFI_USER_RNG;
    for (int i = 0; i < 2; i++) {
        p = gw_wifiown_plan(u, u | GW_WIFI_USER_SCAN);   /* a scan: nothing to do */
        TEST_ASSERT_FALSE(p.init || p.set_mode || p.start || p.stop || p.deinit);
        p = gw_wifiown_plan(u | GW_WIFI_USER_SCAN, u);   /* the scan ends: the driver stays */
        TEST_ASSERT_FALSE(p.stop || p.deinit);
        u = go(&e, u, u | GW_WIFI_USER_AP);              /* the access point: APSTA */
        TEST_ASSERT_EQUAL(GW_WIFI_MODE_APSTA, e.mode);
        u = go(&e, u, GW_WIFI_USER_RNG);                 /* it stops: STA again, not stopped */
        TEST_ASSERT_EQUAL(GW_WIFI_MODE_STA, e.mode);
        TEST_ASSERT_TRUE(e.started);
    }
    TEST_ASSERT_EQUAL(1, e.inits);
    TEST_ASSERT_EQUAL(0, e.deinits);
}

/* Issue #267: the driver posts WIFI_EVENT_STA_START only when the STA interface starts, and its
 * default handler starts the network interface of the station. A station that comes when the STA
 * interface runs (the RNG or a scan has it) is refused; after the access point alone it is not. */
static void test_the_station_comes_first(void)
{
    gw_wifiown_plan_t p = gw_wifiown_plan(GW_WIFI_USER_RNG, GW_WIFI_USER_RNG | GW_WIFI_USER_STA);
    TEST_ASSERT_TRUE(p.refuse);
    TEST_ASSERT_FALSE(p.init || p.set_mode || p.start || p.stop || p.deinit || p.sta_start);
    TEST_ASSERT_EQUAL(GW_WIFI_MODE_STA, p.mode);
    p = gw_wifiown_plan(GW_WIFI_USER_SCAN | GW_WIFI_USER_AP,
                        GW_WIFI_USER_SCAN | GW_WIFI_USER_AP | GW_WIFI_USER_STA);
    TEST_ASSERT_TRUE(p.refuse);
    p = gw_wifiown_plan(GW_WIFI_USER_AP, GW_WIFI_USER_AP | GW_WIFI_USER_STA);
    TEST_ASSERT_FALSE(p.refuse);
    TEST_ASSERT_TRUE(p.set_mode && p.sta_start);
    TEST_ASSERT_EQUAL(GW_WIFI_MODE_APSTA, p.mode);
    p = gw_wifiown_plan(0, GW_WIFI_USER_STA);
    TEST_ASSERT_TRUE(p.sta_start && !p.refuse);
    /* a station that is there already: no refusal (another user comes) */
    p = gw_wifiown_plan(GW_WIFI_USER_STA, GW_WIFI_USER_STA | GW_WIFI_USER_RNG);
    TEST_ASSERT_FALSE(p.refuse);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_modes);
    RUN_TEST(test_scan_with_the_radio_off_gives_the_driver_back);
    RUN_TEST(test_scan_during_the_access_point);
    RUN_TEST(test_overlap_of_the_access_point_and_a_scan);
    RUN_TEST(test_station_keeps_the_driver);
    RUN_TEST(test_the_radio_of_the_rng_never_stops);
    RUN_TEST(test_the_station_comes_first);
    return UNITY_END();
}
