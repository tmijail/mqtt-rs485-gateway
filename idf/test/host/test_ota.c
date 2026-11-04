/* FW-1: health criterion and rollback policy of a new image, update URLs, progress. */
#include <string.h>

#include "gw_ota.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_not_pending_never_acts(void)
{
    gw_ota_trial_t t;
    gw_ota_trial_init(&t, false, 1000, 30000);
    TEST_ASSERT_EQUAL(GW_OTA_TRIAL_NONE, t.state);
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_tick(&t, 1000000));
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_session_up(&t, 0));
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_config_rejected(&t));
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_commissioning(&t));
    TEST_ASSERT_EQUAL_UINT32(0, gw_ota_trial_left_ms(&t, 1000));
}

static void test_first_session_confirms_once(void)
{
    gw_ota_trial_t t;
    gw_ota_trial_init(&t, true, 5000, 30000);
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_tick(&t, 34999));
    TEST_ASSERT_EQUAL_UINT32(1, gw_ota_trial_left_ms(&t, 34999));
    TEST_ASSERT_EQUAL(GW_OTA_CONFIRM, gw_ota_trial_session_up(&t, 0));
    TEST_ASSERT_EQUAL(GW_OTA_TRIAL_CONFIRMING, t.state);   /* until the write succeeds */
    gw_ota_trial_confirm_result(&t, true, 6000, 0);
    TEST_ASSERT_EQUAL(GW_OTA_TRIAL_CONFIRMED, t.state);
    TEST_ASSERT_EQUAL_UINT32(GW_OTA_NO_TICK, gw_ota_trial_next_ms(&t, 6000));
    /* decided: later sessions, the timeout or a rejection change nothing */
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_session_up(&t, 0));
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_tick(&t, 100000));
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_config_rejected(&t));
    TEST_ASSERT_EQUAL_UINT32(0, gw_ota_trial_left_ms(&t, 6000));
}

static void test_timeout_rolls_back(void)
{
    gw_ota_trial_t t;
    gw_ota_trial_init(&t, true, 5000, 30000);
    TEST_ASSERT_EQUAL(GW_OTA_ROLLBACK, gw_ota_trial_tick(&t, 35000));
    TEST_ASSERT_EQUAL(GW_OTA_TRIAL_ROLLBACK, t.state);
    TEST_ASSERT_EQUAL_STRING("no Sparkplug session before the timeout", t.why);
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_session_up(&t, 0));   /* too late */
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_tick(&t, 36000));
}

static void test_timeout_across_the_32bit_wrap(void)
{
    gw_ota_trial_t t;
    gw_ota_trial_init(&t, true, 0xFFFFF000u, 30000);
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_tick(&t, 0x00001000u));   /* 8.2 s later */
    TEST_ASSERT_EQUAL_UINT32(30000 - 8192 - 96, gw_ota_trial_left_ms(&t, 0x00001000u + 96));
    TEST_ASSERT_EQUAL(GW_OTA_ROLLBACK, gw_ota_trial_tick(&t, 0xFFFFF000u + 30000u));
}

static void test_rejected_configuration_and_commissioning(void)
{
    gw_ota_trial_t t;
    gw_ota_trial_init(&t, true, 0, 30000);
    TEST_ASSERT_EQUAL(GW_OTA_ROLLBACK, gw_ota_trial_config_rejected(&t));
    TEST_ASSERT_EQUAL_STRING("it rejected the stored configuration", t.why);
    gw_ota_trial_init(&t, true, 0, 30000);
    TEST_ASSERT_EQUAL(GW_OTA_CONFIRM, gw_ota_trial_commissioning(&t));
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_tick(&t, 60000));
}

/* Issue #101: a refusal never hides a running transfer's state, nor the trial of a new image */
static void test_refusal_keeps_transfer_and_trial(void)
{
    gw_ota_trial_t t;
    gw_ota_trial_init(&t, true, 0, 30000);
    TEST_ASSERT_TRUE(gw_ota_trial_open(&t));
    TEST_ASSERT_EQUAL(GW_OTA_REFUSAL_IN_TRIAL, gw_ota_refusal(&t, false));
    TEST_ASSERT_EQUAL(GW_OTA_REFUSAL_QUIET, gw_ota_refusal(&t, true));
    TEST_ASSERT_EQUAL(GW_OTA_CONFIRM, gw_ota_trial_session_up(&t, 0));
    TEST_ASSERT_TRUE(gw_ota_trial_open(&t));   /* confirming: still no other update */
    gw_ota_trial_confirm_result(&t, true, 10, 0);
    TEST_ASSERT_FALSE(gw_ota_trial_open(&t));
    TEST_ASSERT_EQUAL(GW_OTA_REFUSAL_REPORT, gw_ota_refusal(&t, false));
    TEST_ASSERT_EQUAL(GW_OTA_REFUSAL_QUIET, gw_ota_refusal(&t, true));
    gw_ota_trial_init(&t, false, 0, 30000);
    TEST_ASSERT_FALSE(gw_ota_trial_open(&t));
    TEST_ASSERT_EQUAL(GW_OTA_REFUSAL_REPORT, gw_ota_refusal(&t, false));
    gw_ota_trial_init(&t, true, 0, 30000);
    TEST_ASSERT_EQUAL(GW_OTA_ROLLBACK, gw_ota_trial_tick(&t, 30000));
    TEST_ASSERT_EQUAL(GW_OTA_REFUSAL_REPORT, gw_ota_refusal(&t, false));
}

/* Issue #99: a session without every DBIRTH (too large for the broker) does not confirm; the
 * timeout then rolls the image back, saying why */
static void test_incomplete_births_do_not_confirm(void)
{
    gw_ota_trial_t t;
    gw_ota_trial_init(&t, true, 0, 30000);
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_session_up(&t, 1));
    TEST_ASSERT_EQUAL(GW_OTA_TRIAL_PENDING, t.state);
    TEST_ASSERT_EQUAL_UINT32(20000, gw_ota_trial_next_ms(&t, 10000));
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_session_up(&t, 3));
    TEST_ASSERT_EQUAL(GW_OTA_ROLLBACK, gw_ota_trial_tick(&t, 30000));
    TEST_ASSERT_EQUAL_STRING("no complete Sparkplug session (every DBIRTH) before the timeout", t.why);
    /* a later complete session (a rebirth, a new connection) confirms it in time */
    gw_ota_trial_init(&t, true, 0, 30000);
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_session_up(&t, 1));
    TEST_ASSERT_EQUAL(GW_OTA_CONFIRM, gw_ota_trial_session_up(&t, 0));
    TEST_ASSERT_EQUAL_STRING("first Sparkplug session up", t.why);
    /* not on trial: nothing to say */
    gw_ota_trial_init(&t, false, 0, 30000);
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_session_up(&t, 2));
    TEST_ASSERT_EQUAL_STRING("", t.why);
}

/* Issue #99: a failed write to mark the image valid leaves it on trial, without timeout, and is
 * retried with a backoff from 1 s up to 60 s until it succeeds */
static void test_failed_confirmation_is_retried(void)
{
    gw_ota_trial_t t;
    gw_ota_trial_init(&t, true, 0, 30000);
    TEST_ASSERT_EQUAL(GW_OTA_CONFIRM, gw_ota_trial_session_up(&t, 0));
    TEST_ASSERT_EQUAL_UINT32(GW_OTA_NO_TICK, gw_ota_trial_next_ms(&t, 100));   /* in flight */
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_tick(&t, 100));
    gw_ota_trial_confirm_result(&t, false, 1000, 0);
    TEST_ASSERT_EQUAL(GW_OTA_TRIAL_CONFIRMING, t.state);
    TEST_ASSERT_EQUAL_UINT32(1, t.attempts);
    TEST_ASSERT_EQUAL_UINT32(1000, gw_ota_trial_next_ms(&t, 1000));
    TEST_ASSERT_EQUAL_UINT32(0, gw_ota_trial_left_ms(&t, 1000));   /* no rollback deadline */
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_tick(&t, 1999));
    TEST_ASSERT_EQUAL(GW_OTA_CONFIRM, gw_ota_trial_tick(&t, 2000));
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_tick(&t, 2001));   /* one write at a time */
    uint32_t now = 2000, prev = 1000;
    for (int i = 2; i <= 10; i++) {
        gw_ota_trial_confirm_result(&t, false, now, 0);
        uint32_t d = gw_ota_trial_next_ms(&t, now);
        TEST_ASSERT_TRUE(d >= prev && d <= GW_OTA_RETRY_MAX_MS);
        prev = d;
        now += d;
        TEST_ASSERT_EQUAL(GW_OTA_CONFIRM, gw_ota_trial_tick(&t, now));
    }
    TEST_ASSERT_EQUAL_UINT32(GW_OTA_RETRY_MAX_MS, prev);
    /* the timeout of the trial does not roll back an image judged healthy */
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_tick(&t, now + 100000));
    gw_ota_trial_confirm_result(&t, true, now, 0);
    TEST_ASSERT_EQUAL(GW_OTA_TRIAL_CONFIRMED, t.state);
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_tick(&t, now + 200000));
    /* across the 32-bit wrap of the clock */
    gw_ota_trial_init(&t, true, 0xFFFFF000u, 30000);
    TEST_ASSERT_EQUAL(GW_OTA_CONFIRM, gw_ota_trial_session_up(&t, 0));
    gw_ota_trial_confirm_result(&t, false, 0xFFFFFF00u, 0);
    TEST_ASSERT_EQUAL_UINT32(1000, gw_ota_trial_next_ms(&t, 0xFFFFFF00u));
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_tick(&t, 0x000002E7u));
    TEST_ASSERT_EQUAL(GW_OTA_CONFIRM, gw_ota_trial_tick(&t, 0x000002E8u));
}

/* Issue #99: a save or a factory reset during the trial confirms the image (the restart would
 * roll it back); it retries a failed write at once; no-op once decided or without a trial. An
 * image that cannot start at all is rolled back at once. */
static void test_operator_commit_and_cannot_start(void)
{
    gw_ota_trial_t t;
    gw_ota_trial_init(&t, true, 0, 30000);
    TEST_ASSERT_EQUAL(GW_OTA_CONFIRM, gw_ota_trial_operator(&t, "the operator saved a configuration"));
    TEST_ASSERT_EQUAL_STRING("the operator saved a configuration", t.why);
    gw_ota_trial_confirm_result(&t, false, 10, 0);
    TEST_ASSERT_EQUAL(GW_OTA_CONFIRM, gw_ota_trial_operator(&t, "factory reset"));
    TEST_ASSERT_EQUAL_STRING("the operator saved a configuration", t.why);
    gw_ota_trial_confirm_result(&t, true, 20, 0);
    TEST_ASSERT_EQUAL(GW_OTA_TRIAL_CONFIRMED, t.state);
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_operator(&t, "again"));
    gw_ota_trial_init(&t, false, 0, 30000);
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_operator(&t, "x"));
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_cannot_start(&t, "x"));
    gw_ota_trial_init(&t, true, 0, 30000);
    TEST_ASSERT_EQUAL(GW_OTA_ROLLBACK, gw_ota_trial_cannot_start(&t, "no memory for the configuration"));
    TEST_ASSERT_EQUAL(GW_OTA_TRIAL_ROLLBACK, t.state);
    TEST_ASSERT_EQUAL_STRING("no memory for the configuration", t.why);
    TEST_ASSERT_EQUAL(GW_OTA_KEEP, gw_ota_trial_operator(&t, "x"));   /* rolling back already */
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
    const char *ok = "{\"url\": \"https://fw.example/gw.bin\"}";
    TEST_ASSERT_TRUE(gw_ota_parse_request(ok, strlen(ok), url, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("https://fw.example/gw.bin", url);
    const char *esc = "{\"url\":\"https://fw.example/a\\/b\"}";   /* JSON escape of '/' */
    TEST_ASSERT_TRUE(gw_ota_parse_request(esc, strlen(esc), url, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("https://fw.example/a/b", url);
    const char *bad[] = {"", "[]", "\"https://x\"", "{}", "{\"url\":1}",
                         "{\"url\":\"https://x\",\"force\":true}", "{\"url\":\"https://x\""};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(gw_ota_parse_request(bad[i], strlen(bad[i]), url, why, sizeof(why)),
                                  bad[i]);
    }
    /* not NUL terminated: only len bytes count */
    const char trunc[] = "{\"url\":\"https://x/y\"}garbage";
    TEST_ASSERT_TRUE(gw_ota_parse_request(trunc, sizeof(trunc) - 1 - 7, url, why, sizeof(why)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_not_pending_never_acts);
    RUN_TEST(test_first_session_confirms_once);
    RUN_TEST(test_timeout_rolls_back);
    RUN_TEST(test_timeout_across_the_32bit_wrap);
    RUN_TEST(test_rejected_configuration_and_commissioning);
    RUN_TEST(test_refusal_keeps_transfer_and_trial);
    RUN_TEST(test_incomplete_births_do_not_confirm);
    RUN_TEST(test_failed_confirmation_is_retried);
    RUN_TEST(test_operator_commit_and_cannot_start);
    RUN_TEST(test_urls);
    RUN_TEST(test_progress_every_ten_percent);
    RUN_TEST(test_progress_unknown_size_by_time);
    RUN_TEST(test_request_body);
    return UNITY_END();
}
