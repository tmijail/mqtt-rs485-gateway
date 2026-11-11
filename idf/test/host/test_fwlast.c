/* FW-1 (issue #144 items 5 and 7): the record of the last update and the state texts. */
#include <string.h>

#include "gw_fwlast.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static gw_fwlast_t rec(gw_fwlast_result_t r, gw_ota_mode_t m, const char *v, const char *c)
{
    gw_fwlast_t x;
    memset(&x, 0, sizeof(x));
    x.result = r;
    x.expect = m;
    strcpy(x.version, v);
    strcpy(x.cause, c);
    return x;
}

static void test_format_and_parse(void)
{
    char s[GW_FWLAST_MAX];
    gw_fwlast_t r = rec(GW_FWLAST_RESTARTING, GW_OTA_MODE_NORMAL, "1.2.3", "");
    TEST_ASSERT_EQUAL(18, gw_fwlast_format(&r, s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("r=s;m=n;v=1.2.3;c=", s);
    gw_fwlast_t back;
    TEST_ASSERT_TRUE(gw_fwlast_parse(s, &back));
    TEST_ASSERT_EQUAL(GW_FWLAST_RESTARTING, back.result);
    TEST_ASSERT_EQUAL(GW_OTA_MODE_NORMAL, back.expect);
    TEST_ASSERT_EQUAL_STRING("1.2.3", back.version);
    TEST_ASSERT_EQUAL_STRING("", back.cause);
    /* every result, and a cause with ';' (the rest of the string) */
    const gw_fwlast_result_t all[] = {GW_FWLAST_RESTARTING, GW_FWLAST_TESTING,
                                      GW_FWLAST_INSTALLED, GW_FWLAST_FAILED, GW_FWLAST_REJECTED,
                                      GW_FWLAST_ROLLED_BACK};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        r = rec(all[i], GW_OTA_MODE_RECOVERY, "e2e-v2", "a; b=c; d");
        gw_fwlast_format(&r, s, sizeof(s));
        TEST_ASSERT_TRUE(gw_fwlast_parse(s, &back));
        TEST_ASSERT_EQUAL(all[i], back.result);
        TEST_ASSERT_EQUAL(GW_OTA_MODE_RECOVERY, back.expect);
        TEST_ASSERT_EQUAL_STRING("a; b=c; d", back.cause);
    }
    /* a ';' in the version cannot end it early */
    r = rec(GW_FWLAST_INSTALLED, GW_OTA_MODE_NORMAL, "v;1", "");
    gw_fwlast_format(&r, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("r=i;m=n;v=v_1;c=", s);
}

/* at most 95 characters: the cause is cut, never in the middle of a UTF-8 character */
static void test_length_limit(void)
{
    char s[GW_FWLAST_MAX + 20];
    gw_fwlast_t r = rec(GW_FWLAST_ROLLED_BACK, GW_OTA_MODE_NORMAL,
                        "0123456789012345678901234567890", "");
    memset(r.cause, 'x', 80);
    r.cause[80] = '\0';
    size_t n = gw_fwlast_format(&r, s, sizeof(s));
    TEST_ASSERT_EQUAL(GW_FWLAST_MAX - 1, n);
    TEST_ASSERT_EQUAL(GW_FWLAST_MAX - 1, strlen(s));
    /* "é" (2 bytes) across the limit is dropped whole */
    size_t head = strlen("r=b;m=n;v=0123456789012345678901234567890;c=");
    size_t room = GW_FWLAST_MAX - 1 - head;
    memset(r.cause, 'y', room - 1);
    memcpy(r.cause + room - 1, "\xc3\xa9zz", 5);
    n = gw_fwlast_format(&r, s, sizeof(s));
    TEST_ASSERT_EQUAL(GW_FWLAST_MAX - 2, n);
    TEST_ASSERT_EQUAL_CHAR('y', s[n - 1]);
    /* a small buffer */
    char small[10];
    n = gw_fwlast_format(&r, small, sizeof(small));
    TEST_ASSERT_EQUAL(9, n);
    TEST_ASSERT_EQUAL(0, gw_fwlast_format(&r, small, 0));
}

/* tolerant: unknown keys of a newer image, missing keys, no record */
static void test_parse_is_tolerant(void)
{
    gw_fwlast_t r;
    TEST_ASSERT_TRUE(gw_fwlast_parse("x=1;r=b;q=;v=old;zz=9;c=configuration rejected: a.b", &r));
    TEST_ASSERT_EQUAL(GW_FWLAST_ROLLED_BACK, r.result);
    TEST_ASSERT_EQUAL(GW_OTA_MODE_RECOVERY, r.expect);   /* no "m": recovery */
    TEST_ASSERT_EQUAL_STRING("old", r.version);
    TEST_ASSERT_EQUAL_STRING("configuration rejected: a.b", r.cause);
    TEST_ASSERT_TRUE(gw_fwlast_parse("r=i", &r));
    TEST_ASSERT_EQUAL(GW_FWLAST_INSTALLED, r.result);
    const char *bad[] = {"", "garbage", "r=;v=1", "r=zz;v=1", "r=q;c=x", "v=1;c=r=i"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(gw_fwlast_parse(bad[i], &r), bad[i]);
        TEST_ASSERT_EQUAL(GW_FWLAST_NONE, r.result);
    }
}

static void boot(gw_fwlast_t *r, bool pending, const char *running, bool other_rejected,
                 const char *reset, bool want_write, gw_ota_mode_t want_expected)
{
    const gw_fwlast_boot_t b = {.pending = pending, .running = running,
                                .other_rejected = other_rejected, .reset = reset};
    gw_ota_mode_t expected = GW_OTA_MODE_COMMISSIONING;
    TEST_ASSERT_EQUAL(want_write, gw_fwlast_on_boot(r, &b, &expected));
    TEST_ASSERT_EQUAL(want_expected, expected);
}

/* the new image on trial takes its expected mode from the record of its update */
static void test_boot_of_a_pending_image(void)
{
    gw_fwlast_t r = rec(GW_FWLAST_RESTARTING, GW_OTA_MODE_NORMAL, "v2", "");
    boot(&r, true, "v2", false, "software", true, GW_OTA_MODE_NORMAL);
    TEST_ASSERT_EQUAL(GW_FWLAST_TESTING, r.result);
    boot(&r, true, "v2", false, "software", false, GW_OTA_MODE_NORMAL);   /* written once */
    /* a record about another image (an installer without it): recovery or better */
    r = rec(GW_FWLAST_INSTALLED, GW_OTA_MODE_NORMAL, "v1", "");
    boot(&r, true, "v2", false, "software", true, GW_OTA_MODE_RECOVERY);
    TEST_ASSERT_EQUAL(GW_FWLAST_TESTING, r.result);
    TEST_ASSERT_EQUAL_STRING("v2", r.version);
    gw_fwlast_t none;
    memset(&none, 0, sizeof(none));
    boot(&none, true, "v2", false, "software", true, GW_OTA_MODE_RECOVERY);
}

/* the earlier image after a reset during the test, after a rejection, or after an update that
 * never booted */
static void test_boot_after_a_rollback(void)
{
    gw_fwlast_t r = rec(GW_FWLAST_TESTING, GW_OTA_MODE_NORMAL, "v2", "");
    boot(&r, false, "v1", true, "panic", true, GW_OTA_MODE_NONE);
    TEST_ASSERT_EQUAL(GW_FWLAST_ROLLED_BACK, r.result);
    TEST_ASSERT_EQUAL_STRING("restarted during the test (panic)", r.cause);
    char st[GW_FWLAST_MAX + 40];
    gw_fwlast_state(&r, "v1", st, sizeof(st));
    TEST_ASSERT_EQUAL_STRING("rolled back from v2: restarted during the test (panic)", st);
    boot(&r, false, "v1", true, "software", false, GW_OTA_MODE_NONE);   /* stays */
    gw_fwlast_state(&r, "v1", st, sizeof(st));
    TEST_ASSERT_EQUAL_STRING("rolled back from v2: restarted during the test (panic)", st);

    r = rec(GW_FWLAST_REJECTED, GW_OTA_MODE_NORMAL, "v2", "configuration rejected: a.b");
    boot(&r, false, "v1", true, "software", true, GW_OTA_MODE_NONE);
    TEST_ASSERT_EQUAL(GW_FWLAST_ROLLED_BACK, r.result);
    gw_fwlast_state(&r, "v1", st, sizeof(st));
    TEST_ASSERT_EQUAL_STRING("rolled back from v2: configuration rejected: a.b", st);

    r = rec(GW_FWLAST_RESTARTING, GW_OTA_MODE_NORMAL, "v2", "");
    boot(&r, false, "v1", false, "power-on", true, GW_OTA_MODE_NONE);
    TEST_ASSERT_EQUAL(GW_FWLAST_FAILED, r.result);
    gw_fwlast_state(&r, "v1", st, sizeof(st));
    TEST_ASSERT_EQUAL_STRING("failed: interrupted before the restart (power-on)", st);
    gw_fwlast_t null_reset = rec(GW_FWLAST_TESTING, GW_OTA_MODE_NORMAL, "v2", "");
    const gw_fwlast_boot_t b = {.pending = false, .running = "v1", .other_rejected = true};
    gw_ota_mode_t e;
    TEST_ASSERT_TRUE(gw_fwlast_on_boot(&null_reset, &b, &e));
    TEST_ASSERT_EQUAL_STRING("restarted during the test (?)", null_reset.cause);
}

/* the image of the record runs confirmed: installed */
static void test_boot_of_a_confirmed_image(void)
{
    gw_fwlast_t r = rec(GW_FWLAST_TESTING, GW_OTA_MODE_NORMAL, "v2", "");
    boot(&r, false, "v2", false, "software", true, GW_OTA_MODE_NONE);
    TEST_ASSERT_EQUAL(GW_FWLAST_INSTALLED, r.result);
    char st[GW_FWLAST_MAX + 40];
    gw_fwlast_state(&r, "v2", st, sizeof(st));
    TEST_ASSERT_EQUAL_STRING("installed v2", st);
    boot(&r, false, "v2", false, "power-on", false, GW_OTA_MODE_NONE);
    /* a serial flash of another version since: idle */
    gw_fwlast_state(&r, "v3", st, sizeof(st));
    TEST_ASSERT_EQUAL_STRING("idle", st);
    boot(&r, false, "v3", false, "power-on", false, GW_OTA_MODE_NONE);
    r = rec(GW_FWLAST_FAILED, GW_OTA_MODE_NORMAL, "v2", "image v2 rejected (checksum)");
    boot(&r, false, "v1", false, "software", false, GW_OTA_MODE_NONE);
    gw_fwlast_state(&r, "v1", st, sizeof(st));
    TEST_ASSERT_EQUAL_STRING("failed: image v2 rejected (checksum)", st);
}

static void test_state_texts(void)
{
    char st[GW_FWLAST_MAX + 40];
    gw_fwlast_t r;
    memset(&r, 0, sizeof(r));
    gw_fwlast_state(&r, "v1", st, sizeof(st));
    TEST_ASSERT_EQUAL_STRING("idle", st);
    r = rec(GW_FWLAST_RESTARTING, GW_OTA_MODE_NORMAL, "v2", "");
    gw_fwlast_state(&r, "v1", st, sizeof(st));
    TEST_ASSERT_EQUAL_STRING("restarting into v2", st);
    r = rec(GW_FWLAST_TESTING, GW_OTA_MODE_NORMAL, "v2", "");
    gw_fwlast_state(&r, "v2", st, sizeof(st));
    TEST_ASSERT_EQUAL_STRING("testing v2", st);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_format_and_parse);
    RUN_TEST(test_length_limit);
    RUN_TEST(test_parse_is_tolerant);
    RUN_TEST(test_boot_of_a_pending_image);
    RUN_TEST(test_boot_after_a_rollback);
    RUN_TEST(test_boot_of_a_confirmed_image);
    RUN_TEST(test_state_texts);
    return UNITY_END();
}
