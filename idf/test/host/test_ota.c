/* FW-1: the trial of a new image (issue #144: self-check, expected mode), the pre-flight check
 * of an image, update URLs, progress. */
#include <string.h>

#include "gw_config.h"
#include "gw_ota.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define SC_MS 60000u
#define BACKSTOP_MS 300000u

static const gw_ota_selfcheck_t k_all_ok = {.net_started = true, .web_needed = true,
                                            .web_alive = true};

static void test_expected_mode(void)
{
    /* issue #144 item 2: only an update from normal mode without force must reach normal mode */
    TEST_ASSERT_EQUAL(GW_OTA_MODE_NORMAL, gw_ota_expected_mode(GW_OTA_MODE_NORMAL, false));
    TEST_ASSERT_EQUAL(GW_OTA_MODE_RECOVERY, gw_ota_expected_mode(GW_OTA_MODE_NORMAL, true));
    TEST_ASSERT_EQUAL(GW_OTA_MODE_RECOVERY, gw_ota_expected_mode(GW_OTA_MODE_RECOVERY, false));
    TEST_ASSERT_EQUAL(GW_OTA_MODE_RECOVERY,
                      gw_ota_expected_mode(GW_OTA_MODE_COMMISSIONING, false));
    TEST_ASSERT_TRUE(gw_ota_mode_reached(GW_OTA_MODE_NORMAL, GW_OTA_MODE_NORMAL));
    TEST_ASSERT_FALSE(gw_ota_mode_reached(GW_OTA_MODE_NORMAL, GW_OTA_MODE_RECOVERY));
    TEST_ASSERT_FALSE(gw_ota_mode_reached(GW_OTA_MODE_NORMAL, GW_OTA_MODE_COMMISSIONING));
    TEST_ASSERT_TRUE(gw_ota_mode_reached(GW_OTA_MODE_RECOVERY, GW_OTA_MODE_RECOVERY));
    TEST_ASSERT_TRUE(gw_ota_mode_reached(GW_OTA_MODE_RECOVERY, GW_OTA_MODE_COMMISSIONING));
    TEST_ASSERT_TRUE(gw_ota_mode_reached(GW_OTA_MODE_RECOVERY, GW_OTA_MODE_NORMAL));
    TEST_ASSERT_FALSE(gw_ota_mode_reached(GW_OTA_MODE_RECOVERY, GW_OTA_MODE_NONE));
    TEST_ASSERT_EQUAL_STRING("recovery", gw_ota_mode_name(GW_OTA_MODE_RECOVERY));
    TEST_ASSERT_EQUAL_STRING("normal", gw_ota_mode_name(GW_OTA_MODE_NORMAL));
}

static void test_not_pending_never_acts(void)
{
    gw_ota_trial_t t;
    gw_ota_trial_init(&t, false, GW_OTA_MODE_NORMAL, SC_MS, BACKSTOP_MS);
    TEST_ASSERT_EQUAL(GW_OTA_TRIAL_NONE, t.state);
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_mode(&t, GW_OTA_MODE_RECOVERY, "x"));
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_check(&t, 1000000, &k_all_ok));
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_cannot_start(&t, "no memory to start"));
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_backstop(&t, "mode not reached"));
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_before_restart(&t));
    TEST_ASSERT_FALSE(gw_ota_trial_open(&t));
    TEST_ASSERT_EQUAL_UINT32(0, gw_ota_trial_left_ms(&t, 1000));
}

/* issue #144 item 1: the self-check confirms at the first instant when all items are true, at
 * or after 60 s; a broker, Modbus slaves or NTP do not take part */
static void test_selfcheck_confirms_at_60_s(void)
{
    gw_ota_trial_t t;
    gw_ota_trial_init(&t, true, GW_OTA_MODE_NORMAL, SC_MS, BACKSTOP_MS);
    TEST_ASSERT_TRUE(gw_ota_trial_open(&t));
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_check(&t, 1000, &k_all_ok));   /* mode not set */
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_mode(&t, GW_OTA_MODE_NORMAL, NULL));
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_check(&t, SC_MS - 1, &k_all_ok));
    TEST_ASSERT_EQUAL_UINT32(1, gw_ota_trial_left_ms(&t, SC_MS - 1));
    char why[96];
    gw_ota_trial_refusal(&t, "v2", 15000, why, sizeof(why));
    TEST_ASSERT_EQUAL_STRING("testing v2: 45 s left", why);
    TEST_ASSERT_EQUAL(GW_OTA_CONFIRM, gw_ota_trial_check(&t, SC_MS, &k_all_ok));
    TEST_ASSERT_EQUAL(GW_OTA_TRIAL_CONFIRMING, t.state);   /* until the write succeeds */
    TEST_ASSERT_TRUE(gw_ota_trial_open(&t));
    gw_ota_trial_refusal(&t, "v2", SC_MS, why, sizeof(why));
    TEST_ASSERT_EQUAL_STRING("testing v2: not marked valid yet", why);
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_check(&t, SC_MS + 1000, &k_all_ok));  /* in flight */
    gw_ota_trial_confirm_result(&t, true, SC_MS + 10, 0);
    TEST_ASSERT_EQUAL(GW_OTA_TRIAL_CONFIRMED, t.state);
    TEST_ASSERT_FALSE(gw_ota_trial_open(&t));
    /* decided once */
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_check(&t, BACKSTOP_MS * 2, &k_all_ok));
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_mode(&t, GW_OTA_MODE_RECOVERY, "a"));
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_backstop(&t, "x"));
    TEST_ASSERT_EQUAL(GW_OTA_TRIAL_CONFIRMED, t.state);
}

/* an item that becomes true after 60 s confirms then; the web server only counts when one runs */
static void test_selfcheck_items_and_late_confirmation(void)
{
    gw_ota_trial_t t;
    gw_ota_trial_init(&t, true, GW_OTA_MODE_RECOVERY, SC_MS, BACKSTOP_MS);
    gw_ota_trial_mode(&t, GW_OTA_MODE_RECOVERY, NULL);
    gw_ota_selfcheck_t sc = {.net_started = false, .web_needed = true, .web_alive = true};
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_check(&t, SC_MS, &sc));
    TEST_ASSERT_EQUAL_UINT32(BACKSTOP_MS - SC_MS - 1000, gw_ota_trial_left_ms(&t, SC_MS + 1000));
    sc.net_started = true;
    sc.web_alive = false;
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_check(&t, 120000, &sc));
    sc.web_needed = false;   /* web.enabled false: not an item */
    TEST_ASSERT_EQUAL(GW_OTA_CONFIRM, gw_ota_trial_check(&t, 130000, &sc));
}

/* the backstop: not true after 5 min, rejected with the first item that is false */
static void test_backstop_rejects_with_the_item(void)
{
    const struct {
        bool mode;
        gw_ota_selfcheck_t sc;
        const char *cause;
    } c[] = {
        {false, {true, true, true}, "self-check failed: mode not reached"},
        {true, {false, true, true}, "self-check failed: network driver not started"},
        {true, {true, true, false}, "self-check failed: web server not alive"},
    };
    for (size_t i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
        gw_ota_trial_t t;
        gw_ota_trial_init(&t, true, GW_OTA_MODE_NORMAL, SC_MS, BACKSTOP_MS);
        if (c[i].mode) {
            gw_ota_trial_mode(&t, GW_OTA_MODE_NORMAL, NULL);
        }
        TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_check(&t, BACKSTOP_MS - 1, &c[i].sc));
        TEST_ASSERT_EQUAL(GW_OTA_REJECT, gw_ota_trial_check(&t, BACKSTOP_MS, &c[i].sc));
        TEST_ASSERT_EQUAL(GW_OTA_TRIAL_REJECTED, t.state);
        TEST_ASSERT_EQUAL_STRING(c[i].cause, t.cause);
        TEST_ASSERT_TRUE(gw_ota_trial_open(&t));   /* nothing saved until the restart */
        TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_check(&t, BACKSTOP_MS + 1, &k_all_ok));
        char why[128];
        gw_ota_trial_refusal(&t, "v9", BACKSTOP_MS, why, sizeof(why));
        TEST_ASSERT_EQUAL_STRING_LEN("v9 was rejected (self-check failed: ", why, 36);
    }
    /* the timer of a blocked main task */
    gw_ota_trial_t t;
    gw_ota_trial_init(&t, true, GW_OTA_MODE_NORMAL, SC_MS, BACKSTOP_MS);
    TEST_ASSERT_EQUAL(GW_OTA_REJECT, gw_ota_trial_backstop(&t, "main task blocked"));
    TEST_ASSERT_EQUAL_STRING("self-check failed: main task blocked", t.cause);
    /* a backstop below the self-check time is raised to it */
    gw_ota_trial_init(&t, true, GW_OTA_MODE_NORMAL, SC_MS, 1000);
    TEST_ASSERT_EQUAL_UINT32(SC_MS, t.backstop_ms);
}

/* issue #144 item 2: a rejected configuration after an update from normal mode rejects the
 * image at once, with the path; from recovery (or forced) it stays */
static void test_rejected_configuration(void)
{
    gw_ota_trial_t t;
    gw_ota_trial_init(&t, true, GW_OTA_MODE_NORMAL, SC_MS, BACKSTOP_MS);
    TEST_ASSERT_EQUAL(GW_OTA_REJECT,
                      gw_ota_trial_mode(&t, GW_OTA_MODE_RECOVERY, "devices[0].tags[1].type"));
    TEST_ASSERT_EQUAL_STRING("configuration rejected: devices[0].tags[1].type", t.cause);
    gw_ota_trial_init(&t, true, GW_OTA_MODE_NORMAL, SC_MS, BACKSTOP_MS);
    TEST_ASSERT_EQUAL(GW_OTA_REJECT, gw_ota_trial_mode(&t, GW_OTA_MODE_COMMISSIONING, NULL));
    TEST_ASSERT_EQUAL_STRING("configuration rejected: commissioning", t.cause);
    gw_ota_trial_init(&t, true, GW_OTA_MODE_NORMAL, SC_MS, BACKSTOP_MS);
    TEST_ASSERT_EQUAL(GW_OTA_REJECT, gw_ota_trial_mode(&t, GW_OTA_MODE_RECOVERY, ""));
    TEST_ASSERT_EQUAL_STRING("configuration rejected: ?", t.cause);
    /* expected recovery: it stays, and the self-check confirms it in recovery mode */
    gw_ota_trial_init(&t, true, GW_OTA_MODE_RECOVERY, SC_MS, BACKSTOP_MS);
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_mode(&t, GW_OTA_MODE_RECOVERY, "a.b"));
    TEST_ASSERT_EQUAL(GW_OTA_CONFIRM, gw_ota_trial_check(&t, SC_MS, &k_all_ok));
    /* an expected mode other than normal counts as recovery */
    gw_ota_trial_init(&t, true, GW_OTA_MODE_COMMISSIONING, SC_MS, BACKSTOP_MS);
    TEST_ASSERT_EQUAL(GW_OTA_MODE_RECOVERY, t.expected);
}

static void test_cannot_start(void)
{
    gw_ota_trial_t t;
    gw_ota_trial_init(&t, true, GW_OTA_MODE_RECOVERY, SC_MS, BACKSTOP_MS);
    TEST_ASSERT_EQUAL(GW_OTA_REJECT, gw_ota_trial_cannot_start(&t, "no memory to start"));
    TEST_ASSERT_EQUAL_STRING("no memory to start", t.cause);
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_cannot_start(&t, "again"));
    TEST_ASSERT_EQUAL_STRING("no memory to start", t.cause);
}

/* issue #99: a failed write keeps the image on trial (no backstop: it passed the self-check),
 * retried after 1, 2, 4 ... up to 60 s, and once before a planned restart */
static void test_failed_confirmation_is_retried(void)
{
    gw_ota_trial_t t;
    gw_ota_trial_init(&t, true, GW_OTA_MODE_NORMAL, SC_MS, BACKSTOP_MS);
    gw_ota_trial_mode(&t, GW_OTA_MODE_NORMAL, NULL);
    TEST_ASSERT_EQUAL(GW_OTA_CONFIRM, gw_ota_trial_check(&t, SC_MS, &k_all_ok));
    gw_ota_trial_confirm_result(&t, false, SC_MS, 0);
    TEST_ASSERT_EQUAL(GW_OTA_TRIAL_CONFIRMING, t.state);
    TEST_ASSERT_EQUAL_UINT32(1, t.attempts);
    TEST_ASSERT_EQUAL_UINT32(0, gw_ota_trial_left_ms(&t, SC_MS));
    uint32_t due = t.retry_at_ms;
    TEST_ASSERT_TRUE(due > SC_MS && due <= SC_MS + GW_OTA_RETRY_MIN_MS);
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_check(&t, due - 1, &k_all_ok));
    TEST_ASSERT_EQUAL(GW_OTA_CONFIRM, gw_ota_trial_check(&t, due, &k_all_ok));
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_before_restart(&t));   /* one in flight */
    uint32_t now = due;
    for (int i = 0; i < 12; i++) {
        gw_ota_trial_confirm_result(&t, false, now, 0xFFFFFFFFu);
        TEST_ASSERT_TRUE(t.retry_at_ms - now <= GW_OTA_RETRY_MAX_MS);
        now = t.retry_at_ms;
        TEST_ASSERT_EQUAL(GW_OTA_CONFIRM, gw_ota_trial_check(&t, now, &k_all_ok));
    }
    /* far past the backstop: still not rejected */
    TEST_ASSERT_TRUE(now > BACKSTOP_MS);
    gw_ota_trial_confirm_result(&t, false, now, 0);
    TEST_ASSERT_EQUAL(GW_OTA_TRIAL_CONFIRMING, t.state);
    /* before a planned restart: one more try at once */
    TEST_ASSERT_EQUAL(GW_OTA_CONFIRM, gw_ota_trial_before_restart(&t));
    gw_ota_trial_confirm_result(&t, true, now, 0);
    TEST_ASSERT_EQUAL(GW_OTA_TRIAL_CONFIRMED, t.state);
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_before_restart(&t));
    /* a result that comes after the state changed changes nothing */
    gw_ota_trial_confirm_result(&t, false, now, 0);
    TEST_ASSERT_EQUAL(GW_OTA_TRIAL_CONFIRMED, t.state);
}

static void test_uptime_past_32_bits_in_retries(void)
{
    gw_ota_trial_t t;
    gw_ota_trial_init(&t, true, GW_OTA_MODE_NORMAL, SC_MS, BACKSTOP_MS);
    gw_ota_trial_mode(&t, GW_OTA_MODE_NORMAL, NULL);
    gw_ota_trial_check(&t, SC_MS, &k_all_ok);
    gw_ota_trial_confirm_result(&t, false, 0xFFFFFF00u, 0);   /* the retry wraps */
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_check(&t, 0xFFFFFF80u, &k_all_ok));
    TEST_ASSERT_EQUAL(GW_OTA_CONFIRM, gw_ota_trial_check(&t, t.retry_at_ms, &k_all_ok));
}

static void caps_block(uint8_t *b, uint16_t version, uint16_t size, uint32_t flags,
                       uint16_t schema, uint16_t nvs, uint32_t lfs)
{
    memset(b, 0, GW_OTA_CAPS_SIZE);
    const uint8_t m[4] = {0x47, 0x57, 0x43, 0x50};
    memcpy(b, m, 4);
    b[4] = (uint8_t)version;
    b[5] = (uint8_t)(version >> 8);
    b[6] = (uint8_t)size;
    b[7] = (uint8_t)(size >> 8);
    for (int i = 0; i < 4; i++) {
        b[8 + i] = (uint8_t)(flags >> (8 * i));
        b[16 + i] = (uint8_t)(lfs >> (8 * i));
    }
    b[12] = (uint8_t)schema;
    b[13] = (uint8_t)(schema >> 8);
    b[14] = (uint8_t)nvs;
    b[15] = (uint8_t)(nvs >> 8);
}

static void test_image_capabilities(void)
{
    uint8_t b[GW_OTA_CAPS_SIZE];
    gw_ota_caps_info_t c;
    caps_block(b, 1, 32, GW_OTA_CAP_NVS_ENCRYPTION, 0, 0, 0);
    gw_ota_caps_parse(b, sizeof(b), &c);
    TEST_ASSERT_TRUE(c.declared);
    TEST_ASSERT_EQUAL_UINT16(1, c.version);
    TEST_ASSERT_EQUAL_HEX32(GW_OTA_CAP_NVS_ENCRYPTION, c.flags);
    TEST_ASSERT_EQUAL_UINT16(0, c.cfg_schema);   /* version 1: no schema */
    /* version 2 (#144, #148) */
    caps_block(b, 2, 32, GW_OTA_CAP_FLASH_ENCRYPTION, 7, 0x0102, 0x00020001);
    gw_ota_caps_parse(b, sizeof(b), &c);
    TEST_ASSERT_TRUE(c.declared);
    TEST_ASSERT_EQUAL_HEX32(GW_OTA_CAP_FLASH_ENCRYPTION, c.flags);
    TEST_ASSERT_EQUAL_UINT16(7, c.cfg_schema);
    TEST_ASSERT_EQUAL_HEX16(0x0102, c.nvs_format);
    TEST_ASSERT_EQUAL_HEX32(0x00020001, c.lfs_disk);
    /* a version 2 block shorter than its fields: only the first 12 bytes */
    b[6] = 12;
    gw_ota_caps_parse(b, sizeof(b), &c);
    TEST_ASSERT_TRUE(c.declared);
    TEST_ASSERT_EQUAL_UINT16(0, c.cfg_schema);
    /* a newer version keeps the offsets */
    caps_block(b, 9, 32, 0, 3, 0x0101, 0x00020001);
    gw_ota_caps_parse(b, sizeof(b), &c);
    TEST_ASSERT_EQUAL_UINT16(3, c.cfg_schema);
    b[6] = 33;   /* larger than what was read */
    gw_ota_caps_parse(b, sizeof(b), &c);
    TEST_ASSERT_FALSE(c.declared);
    b[6] = 11;
    gw_ota_caps_parse(b, sizeof(b), &c);
    TEST_ASSERT_FALSE(c.declared);
    b[6] = 32;
    b[4] = 0;
    b[5] = 0;
    gw_ota_caps_parse(b, sizeof(b), &c);
    TEST_ASSERT_FALSE(c.declared);
    b[4] = 1;
    b[0] = 0x48;   /* other magic: an image built before #102 (its .rodata goes on there) */
    gw_ota_caps_parse(b, sizeof(b), &c);
    TEST_ASSERT_FALSE(c.declared);
    b[0] = 0x47;
    gw_ota_caps_parse(b, 11, &c);
    TEST_ASSERT_FALSE(c.declared);
    /* the layout the firmware compiles */
    TEST_ASSERT_EQUAL(GW_OTA_CAPS_SIZE, sizeof(gw_ota_caps_t));

    /* issue #102: the NVS encryption rule */
    char why[96] = "";
    gw_ota_caps_info_t img = {.declared = true, .version = 2, .flags = GW_OTA_CAP_NVS_ENCRYPTION};
    TEST_ASSERT_TRUE(gw_ota_caps_accept(&img, true, why, sizeof(why)));
    img.flags = 0;
    TEST_ASSERT_TRUE(gw_ota_caps_accept(&img, false, why, sizeof(why)));   /* NVS not encrypted */
    gw_ota_caps_info_t none = {0};
    TEST_ASSERT_TRUE(gw_ota_caps_accept(&none, false, why, sizeof(why)));   /* an older image */
    img.flags = GW_OTA_CAP_FLASH_ENCRYPTION;
    TEST_ASSERT_FALSE(gw_ota_caps_accept(&img, true, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("built without NVS encryption (this device's NVS is encrypted)", why);
    TEST_ASSERT_FALSE(gw_ota_caps_accept(&none, true, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("declares no NVS encryption (this device's NVS is encrypted)", why);
}

static void report_with(gw_schema_report_t *r, const gw_since_t *table, size_t n)
{
    gw_schema_init(r, table, n);
    gw_schema_note(r, "network", NULL);
    gw_schema_note(r, "network.dhcp", NULL);
    gw_schema_note(r, "devices[0].tags[3].scan", NULL);
    gw_schema_note(r, "sparkplug.naming", "umh");
    r->valid = true;
}

/* issue #144 item 3 and #148: the pre-flight check */
static void test_preflight(void)
{
    static const gw_since_t table[] = {{"devices[].tags[].scan", NULL, 3},
                                       {"sparkplug.naming", "umh", 2}};
    gw_schema_report_t doc;
    report_with(&doc, table, 2);
    TEST_ASSERT_EQUAL_UINT16(3, doc.need);
    const gw_ota_caps_info_t own = {.declared = true, .version = 2, .cfg_schema = 3,
                                    .nvs_format = 0x0101, .lfs_disk = 0x00020001};
    gw_ota_caps_info_t img = own;
    char why[256];
    TEST_ASSERT_EQUAL(GW_OTA_PF_OK, gw_ota_preflight(&own, &img, &doc, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("", why);
    /* a downgrade that cannot read the configuration: the paths that are too new */
    img.cfg_schema = 1;
    TEST_ASSERT_EQUAL(GW_OTA_PF_REFUSE, gw_ota_preflight(&own, &img, &doc, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("the configuration needs schema 3, the image reads 1: "
                             "devices[0].tags[3].scan (3), sparkplug.naming=umh (2)", why);
    img.cfg_schema = 2;
    TEST_ASSERT_EQUAL(GW_OTA_PF_REFUSE, gw_ota_preflight(&own, &img, &doc, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("the configuration needs schema 3, the image reads 2: "
                             "devices[0].tags[3].scan (3)", why);
    /* a configuration that the running image does not load: nothing to check */
    TEST_ASSERT_EQUAL(GW_OTA_PF_OK, gw_ota_preflight(&own, &img, NULL, why, sizeof(why)));
    doc.valid = false;
    TEST_ASSERT_EQUAL(GW_OTA_PF_OK, gw_ota_preflight(&own, &img, &doc, why, sizeof(why)));
    doc.valid = true;
    /* NVS format: the IDF page format or the gateway records of the image are older */
    img = own;
    img.nvs_format = 0x0100;
    TEST_ASSERT_EQUAL(GW_OTA_PF_REFUSE, gw_ota_preflight(&own, &img, &doc, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("NVS format 1.0, this firmware writes 1.1", why);
    img.nvs_format = 0x0201;   /* newer: it reads ours */
    TEST_ASSERT_EQUAL(GW_OTA_PF_OK, gw_ota_preflight(&own, &img, &doc, why, sizeof(why)));
    img.nvs_format = 0x0002;
    TEST_ASSERT_EQUAL(GW_OTA_PF_REFUSE, gw_ota_preflight(&own, &img, &doc, why, sizeof(why)));
    /* LittleFS: another major version or an older minor version */
    img = own;
    img.lfs_disk = 0x00020000;
    TEST_ASSERT_EQUAL(GW_OTA_PF_REFUSE, gw_ota_preflight(&own, &img, &doc, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("LittleFS disk version 2.0, this firmware writes 2.1", why);
    img.lfs_disk = 0x00020002;
    TEST_ASSERT_EQUAL(GW_OTA_PF_OK, gw_ota_preflight(&own, &img, &doc, why, sizeof(why)));
    img.lfs_disk = 0x00030001;
    TEST_ASSERT_EQUAL(GW_OTA_PF_REFUSE, gw_ota_preflight(&own, &img, &doc, why, sizeof(why)));
    /* all reasons together */
    img.cfg_schema = 2;
    img.nvs_format = 0x0100;
    TEST_ASSERT_EQUAL(GW_OTA_PF_REFUSE, gw_ota_preflight(&own, &img, &doc, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("the configuration needs schema 3, the image reads 2: "
                             "devices[0].tags[3].scan (3); NVS format 1.0, this firmware writes "
                             "1.1; LittleFS disk version 3.1, this firmware writes 2.1", why);
    /* an image without capabilities version 2 (all images before #144): unknown, force */
    gw_ota_caps_info_t v1 = {.declared = true, .version = 1, .flags = 1};
    TEST_ASSERT_EQUAL(GW_OTA_PF_UNKNOWN, gw_ota_preflight(&own, &v1, &doc, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("the image declares no configuration schema (capabilities version "
                             "1, built before the pre-flight check): it may not read the stored "
                             "data", why);
    gw_ota_caps_info_t none = {0};
    TEST_ASSERT_EQUAL(GW_OTA_PF_UNKNOWN, gw_ota_preflight(&own, &none, NULL, why, sizeof(why)));
    /* a small buffer: cut, never overrun */
    char small[20];
    img.cfg_schema = 1;
    TEST_ASSERT_EQUAL(GW_OTA_PF_REFUSE, gw_ota_preflight(&own, &img, &doc, small, sizeof(small)));
    TEST_ASSERT_EQUAL(sizeof(small) - 1, strlen(small));
}

/* this firmware's own capabilities pass its own pre-flight with its own configuration */
static void test_preflight_same_image(void)
{
    gw_schema_report_t doc;
    gw_config_schema_init(&doc);
    gw_schema_note(&doc, "network", NULL);
    doc.valid = true;
    const gw_ota_caps_info_t own = {.declared = true, .version = GW_OTA_CAPS_VERSION,
                                    .cfg_schema = GW_CONFIG_SCHEMA,
                                    .nvs_format = 0x0100 | GW_NVS_RECORDS,
                                    .lfs_disk = 0x00020001};
    char why[160];
    TEST_ASSERT_EQUAL(GW_OTA_PF_OK, gw_ota_preflight(&own, &own, &doc, why, sizeof(why)));
}

static void test_urls(void)
{
    char why[96];
    TEST_ASSERT_TRUE(gw_ota_url_ok("https://10.0.2.2:18444/fw/gw.bin", false, why, sizeof(why)));
    TEST_ASSERT_TRUE(gw_ota_url_ok("HTTPS://fw.example/a?b=c", false, why, sizeof(why)));
    TEST_ASSERT_TRUE(gw_ota_url_ok("https://[fe80::1]/gw.bin", false, why, sizeof(why)));
    TEST_ASSERT_FALSE(gw_ota_url_ok("http://fw.example/gw.bin", false, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("only https:// URLs are accepted", why);
    TEST_ASSERT_TRUE(gw_ota_url_ok("http://fw.example/gw.bin", true, why, sizeof(why)));
    const char *bad[] = {"", "ftp://x/y", "https://", "https:///gw.bin", "https://:443/x",
                         "https://fw.example/a b", "https://fw.example/\x01", "fw.example/gw.bin",
                         "https://user:pw@fw.example/gw.bin", "https://fw.example/\xc3\xa9"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        why[0] = '\0';
        TEST_ASSERT_FALSE_MESSAGE(gw_ota_url_ok(bad[i], true, why, sizeof(why)), bad[i]);
        TEST_ASSERT_TRUE(why[0] != '\0');
    }
    /* '@' after the path is not userinfo */
    TEST_ASSERT_TRUE(gw_ota_url_ok("https://fw.example/a@b.bin", false, why, sizeof(why)));
    char longu[GW_OTA_URL_MAX + 8];
    memset(longu, 'a', sizeof(longu));
    memcpy(longu, "https://", 8);
    longu[GW_OTA_URL_MAX] = '\0';   /* 256 characters: one too many */
    TEST_ASSERT_FALSE(gw_ota_url_ok(longu, false, why, sizeof(why)));
    longu[GW_OTA_URL_MAX - 1] = '\0';
    TEST_ASSERT_TRUE(gw_ota_url_ok(longu, false, why, sizeof(why)));
}

static void test_progress_every_ten_percent(void)
{
    gw_ota_progress_t p;
    uint8_t pct;
    gw_ota_progress_init(&p, 1000, 0);
    TEST_ASSERT_TRUE(gw_ota_progress_update(&p, 0, 0, &pct));     /* the start */
    TEST_ASSERT_EQUAL_UINT8(0, pct);
    TEST_ASSERT_FALSE(gw_ota_progress_update(&p, 99, 10, &pct));
    TEST_ASSERT_TRUE(gw_ota_progress_update(&p, 100, 20, &pct));
    TEST_ASSERT_EQUAL_UINT8(10, pct);
    TEST_ASSERT_FALSE(gw_ota_progress_update(&p, 150, 30, &pct));
    TEST_ASSERT_TRUE(gw_ota_progress_update(&p, 450, 40, &pct));  /* jumps are fine */
    TEST_ASSERT_EQUAL_UINT8(45, pct);
    TEST_ASSERT_FALSE(gw_ota_progress_update(&p, 540, 50, &pct));
    TEST_ASSERT_TRUE(gw_ota_progress_update(&p, 1000, 60, &pct));
    TEST_ASSERT_EQUAL_UINT8(100, pct);
    TEST_ASSERT_FALSE(gw_ota_progress_update(&p, 1200, 70, &pct));  /* more than announced */
    TEST_ASSERT_EQUAL_UINT8(100, pct);
    /* 4 GB image: no overflow */
    gw_ota_progress_init(&p, 0xFFFFFFF0u, 0);
    gw_ota_progress_update(&p, 0, 0, &pct);
    TEST_ASSERT_TRUE(gw_ota_progress_update(&p, 0x80000000u, 1, &pct));
    TEST_ASSERT_EQUAL_UINT8(50, pct);
}

static void test_progress_unknown_size_by_time(void)
{
    gw_ota_progress_t p;
    uint8_t pct;
    gw_ota_progress_init(&p, 0, 1000);
    TEST_ASSERT_TRUE(gw_ota_progress_update(&p, 10, 1000, &pct));
    TEST_ASSERT_FALSE(gw_ota_progress_update(&p, 5000, 5999, &pct));
    TEST_ASSERT_TRUE(gw_ota_progress_update(&p, 6000, 6000, &pct));
    TEST_ASSERT_EQUAL_UINT8(0, pct);
    TEST_ASSERT_EQUAL_UINT32(6000, p.done);
}

static void test_request_body(void)
{
    char url[GW_OTA_URL_MAX], why[96];
    bool force = true;
    const char *ok = "{\"url\": \"https://fw.example/gw.bin\"}";
    TEST_ASSERT_TRUE(gw_ota_parse_request(ok, strlen(ok), url, &force, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("https://fw.example/gw.bin", url);
    TEST_ASSERT_FALSE(force);
    const char *esc = "{\"url\":\"https://fw.example/a\\/b\"}";   /* JSON escape of '/' */
    TEST_ASSERT_TRUE(gw_ota_parse_request(esc, strlen(esc), url, &force, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("https://fw.example/a/b", url);
    /* issue #144 item 3: force */
    const char *forced = "{\"url\":\"https://x/y\",\"force\":true}";
    TEST_ASSERT_TRUE(gw_ota_parse_request(forced, strlen(forced), url, &force, why, sizeof(why)));
    TEST_ASSERT_TRUE(force);
    const char *unforced = "{\"force\":false,\"url\":\"https://x/y\"}";
    TEST_ASSERT_TRUE(gw_ota_parse_request(unforced, strlen(unforced), url, &force, why,
                                          sizeof(why)));
    TEST_ASSERT_FALSE(force);
    const char *bad[] = {"", "[]", "\"https://x\"", "{}", "{\"url\":1}",
                         "{\"url\":\"https://x\",\"force\":1}",
                         "{\"url\":\"https://x\",\"other\":true}",
                         "{\"url\":\"https://x\",\"force\":true,\"x\":1}", "{\"force\":true}",
                         "{\"url\":\"https://x\""};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(gw_ota_parse_request(bad[i], strlen(bad[i]), url, &force, why,
                                                       sizeof(why)),
                                  bad[i]);
    }
    /* not NUL terminated: only len bytes count */
    const char trunc[] = "{\"url\":\"https://x/y\"}garbage";
    TEST_ASSERT_TRUE(gw_ota_parse_request(trunc, sizeof(trunc) - 1 - 7, url, &force, why,
                                          sizeof(why)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_expected_mode);
    RUN_TEST(test_not_pending_never_acts);
    RUN_TEST(test_selfcheck_confirms_at_60_s);
    RUN_TEST(test_selfcheck_items_and_late_confirmation);
    RUN_TEST(test_backstop_rejects_with_the_item);
    RUN_TEST(test_rejected_configuration);
    RUN_TEST(test_cannot_start);
    RUN_TEST(test_failed_confirmation_is_retried);
    RUN_TEST(test_uptime_past_32_bits_in_retries);
    RUN_TEST(test_image_capabilities);
    RUN_TEST(test_preflight);
    RUN_TEST(test_preflight_same_image);
    RUN_TEST(test_urls);
    RUN_TEST(test_progress_every_ten_percent);
    RUN_TEST(test_progress_unknown_size_by_time);
    RUN_TEST(test_request_body);
    return UNITY_END();
}
