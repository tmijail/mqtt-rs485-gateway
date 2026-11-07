/* QA-1: issues #139 and #137, the microSD card's state and the format's state machine. */
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "gw_cardfmt.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static cJSON *parse(const char *s)
{
    cJSON *j = cJSON_Parse(s);
    TEST_ASSERT_NOT_NULL_MESSAGE(j, s);
    return j;
}

static const char *str(const cJSON *j, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static double num(const cJSON *j, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    TEST_ASSERT_TRUE_MESSAGE(cJSON_IsNumber(v), key);
    return v->valuedouble;
}

static bool has(const cJSON *j, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(j, key) != NULL;
}

/* One format at a time, before any other refusal; then a card in the build, then the PSRAM. */
static void test_request_order(void)
{
    gw_cardfmt_t st = {0};
    TEST_ASSERT_EQUAL(GW_CARDFMT_NO_SUPPORT, gw_cardfmt_request(&st, false, false, false, 10));
    TEST_ASSERT_EQUAL(GW_CARDFMT_NO_SUPPORT, gw_cardfmt_request(&st, false, true, false, 10));
    TEST_ASSERT_EQUAL(GW_CARDFMT_NO_PSRAM, gw_cardfmt_request(&st, true, false, false, 10));
    TEST_ASSERT_EQUAL(GW_CARDFMT_IDLE, st.phase);   /* a refusal changes nothing */
    TEST_ASSERT_EQUAL_UINT32(0, st.count);
    TEST_ASSERT_EQUAL(GW_CARDFMT_STARTED, gw_cardfmt_request(&st, true, true, false, 10));
    TEST_ASSERT_EQUAL(GW_CARDFMT_RUNNING, st.phase);
    TEST_ASSERT_EQUAL_UINT32(1, st.count);
    TEST_ASSERT_FALSE(st.simulated);
    /* while it runs, every request is "busy", also one that would be refused otherwise */
    TEST_ASSERT_EQUAL(GW_CARDFMT_BUSY, gw_cardfmt_request(&st, true, true, false, 20));
    TEST_ASSERT_EQUAL(GW_CARDFMT_BUSY, gw_cardfmt_request(&st, false, false, false, 20));
    TEST_ASSERT_EQUAL(GW_CARDFMT_BUSY, gw_cardfmt_request(&st, false, false, true, 20));
    TEST_ASSERT_EQUAL_UINT32(10, st.start_ms);
    /* done: a new format may start, and it clears the result of the last one */
    gw_cardfmt_done(&st, 5010, "FAT32", 32768, 61024);
    TEST_ASSERT_EQUAL(GW_CARDFMT_STARTED, gw_cardfmt_request(&st, true, true, false, 6000));
    TEST_ASSERT_EQUAL_UINT32(2, st.count);
    TEST_ASSERT_EQUAL_STRING("", st.fat);
    gw_cardfmt_failed(&st, 6500, "no card answers");
    TEST_ASSERT_EQUAL(GW_CARDFMT_STARTED, gw_cardfmt_request(&st, true, true, false, 7000));
    TEST_ASSERT_EQUAL_STRING("", st.why);
}

/* The emulator test run needs neither a card nor the PSRAM, but it is one format too. */
static void test_simulated(void)
{
    gw_cardfmt_t st = {0};
    TEST_ASSERT_EQUAL(GW_CARDFMT_STARTED, gw_cardfmt_request(&st, false, false, true, 0));
    TEST_ASSERT_TRUE(st.simulated);
    TEST_ASSERT_EQUAL(GW_CARDFMT_BUSY, gw_cardfmt_request(&st, true, true, false, 1));
    char line[200];
    gw_cardfmt_text(&st, 2500, line, sizeof(line));
    TEST_ASSERT_EQUAL_STRING("running for 2 s (simulated: nothing written)", line);
    gw_cardfmt_done(&st, 4000, "FAT32", 32768, 0);
    gw_cardfmt_text(&st, 9999, line, sizeof(line));
    TEST_ASSERT_EQUAL_STRING("done in 4 s (simulated: nothing written): FAT32, 32768 B clusters, "
                             "0 MB; restart the gateway to use the card",
                             line);
}

static void test_elapsed_and_wrap(void)
{
    gw_cardfmt_t st = {0};
    TEST_ASSERT_EQUAL_UINT32(0, gw_cardfmt_elapsed_ms(&st, 12345));
    gw_cardfmt_request(&st, true, true, false, UINT32_MAX - 499);   /* the clock wraps */
    TEST_ASSERT_EQUAL_UINT32(1500, gw_cardfmt_elapsed_ms(&st, 1000));
    gw_cardfmt_failed(&st, 2000, "the Sparkplug task did not release the card in 10 s");
    TEST_ASSERT_EQUAL_UINT32(2500, gw_cardfmt_elapsed_ms(&st, 900000));   /* fixed when ended */
    char line[200];
    gw_cardfmt_text(&st, 0, line, sizeof(line));
    TEST_ASSERT_EQUAL_STRING("FAILED after 2 s: the Sparkplug task did not release the card in 10 s",
                             line);
}

static void test_format_json(void)
{
    gw_cardfmt_t st = {0};
    char buf[300];
    TEST_ASSERT_GREATER_THAN(0, gw_cardfmt_json(&st, 0, buf, sizeof(buf)));
    cJSON *j = parse(buf);
    TEST_ASSERT_EQUAL_STRING("idle", str(j, "state"));
    TEST_ASSERT_FALSE(has(j, "elapsed_s"));
    cJSON_Delete(j);

    gw_cardfmt_request(&st, true, true, false, 1000);
    gw_cardfmt_json(&st, 13999, buf, sizeof(buf));
    j = parse(buf);
    TEST_ASSERT_EQUAL_STRING("running", str(j, "state"));
    TEST_ASSERT_EQUAL_DOUBLE(12, num(j, "elapsed_s"));
    TEST_ASSERT_FALSE(has(j, "simulated"));
    cJSON_Delete(j);

    gw_cardfmt_done(&st, 48000, "FAT32", 32768, 122000);
    gw_cardfmt_json(&st, 99000, buf, sizeof(buf));
    j = parse(buf);
    TEST_ASSERT_EQUAL_STRING("done", str(j, "state"));
    TEST_ASSERT_EQUAL_DOUBLE(47, num(j, "duration_s"));
    TEST_ASSERT_EQUAL_STRING("FAT32", str(j, "fat"));
    TEST_ASSERT_EQUAL_DOUBLE(32768, num(j, "cluster"));
    TEST_ASSERT_EQUAL_DOUBLE(122000, num(j, "size_mb"));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(j, "restart")));
    cJSON_Delete(j);

    /* a reason with quotes, a backslash and a control character stays valid JSON */
    gw_cardfmt_request(&st, true, true, false, 0);
    gw_cardfmt_failed(&st, 3000, "f_mkfs \"aborted\" \\ (14)\n");
    gw_cardfmt_json(&st, 3000, buf, sizeof(buf));
    j = parse(buf);
    TEST_ASSERT_EQUAL_STRING("failed", str(j, "state"));
    TEST_ASSERT_EQUAL_STRING("f_mkfs \"aborted\" \\ (14)\n", str(j, "error"));
    TEST_ASSERT_EQUAL_DOUBLE(3, num(j, "duration_s"));
    cJSON_Delete(j);

    /* too small a buffer: "{}" (valid JSON), never a cut object */
    char small[20];
    TEST_ASSERT_EQUAL_size_t(0, gw_cardfmt_json(&st, 3000, small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("{}", small);
}

/* The reason a long text cannot overflow: it is cut at the field's size. */
static void test_long_reason(void)
{
    gw_cardfmt_t st = {0};
    char why[300];
    memset(why, 'x', sizeof(why) - 1);
    why[sizeof(why) - 1] = '\0';
    gw_cardfmt_request(&st, true, true, false, 0);
    gw_cardfmt_failed(&st, 1, why);
    TEST_ASSERT_EQUAL_size_t(sizeof(st.why) - 1, strlen(st.why));
    gw_cardfmt_done(&st, 1, "FAT32-LONG-NAME", 1, 1);
    TEST_ASSERT_EQUAL_STRING("FAT32", st.fat);
    char line[32];
    gw_cardfmt_text(&st, 1, line, sizeof(line));
    TEST_ASSERT_EQUAL_size_t(sizeof(line) - 1, strlen(line));
}

static void test_answers(void)
{
    TEST_ASSERT_EQUAL_STRING("202 Accepted", gw_cardfmt_answer_http(GW_CARDFMT_STARTED));
    TEST_ASSERT_EQUAL_STRING("409 Conflict", gw_cardfmt_answer_http(GW_CARDFMT_BUSY));
    TEST_ASSERT_EQUAL_STRING("501 Not Implemented", gw_cardfmt_answer_http(GW_CARDFMT_NO_SUPPORT));
    TEST_ASSERT_EQUAL_STRING("501 Not Implemented", gw_cardfmt_answer_http(GW_CARDFMT_NO_PSRAM));
    TEST_ASSERT_EQUAL_STRING("503 Service Unavailable", gw_cardfmt_answer_http(GW_CARDFMT_NO_MEMORY));
    TEST_ASSERT_EQUAL_STRING("no_support", gw_cardfmt_answer_key(GW_CARDFMT_NO_SUPPORT));
    TEST_ASSERT_EQUAL_STRING("no_psram", gw_cardfmt_answer_key(GW_CARDFMT_NO_PSRAM));
    TEST_ASSERT_EQUAL_STRING("busy", gw_cardfmt_answer_key(GW_CARDFMT_BUSY));
    /* the QEMU build's refusal, as the E2E expects it */
    TEST_ASSERT_EQUAL_STRING_LEN("this build has no microSD card", gw_cardfmt_answer_text(GW_CARDFMT_NO_SUPPORT),
                                 30);
    TEST_ASSERT_EQUAL_STRING("?", gw_cardfmt_answer_key((gw_cardfmt_answer_t)99));
    TEST_ASSERT_EQUAL_STRING("?", gw_cardfmt_phase_name((gw_cardfmt_phase_t)99));
    TEST_ASSERT_EQUAL_STRING("?", gw_card_state_name(GW_CARD_STATE_COUNT));
}

static gw_card_info_t card(gw_card_state_t s, const char *detail)
{
    gw_card_info_t c = {.state = s, .free_mb = GW_CARD_UNKNOWN, .other = GW_CARD_UNKNOWN};
    snprintf(c.detail, sizeof(c.detail), "%s", detail);
    return c;
}

static void test_card(void)
{
    char buf[400];
    gw_card_info_t c = card(GW_CARD_UNSUPPORTED, "this build has no microSD card");
    gw_card_json(&c, buf, sizeof(buf));
    cJSON *j = parse(buf);
    TEST_ASSERT_EQUAL_STRING("unsupported", str(j, "state"));
    TEST_ASSERT_EQUAL_STRING("this build has no microSD card", str(j, "detail"));
    /* what is not known is left out */
    TEST_ASSERT_FALSE(has(j, "size_mb"));
    TEST_ASSERT_FALSE(has(j, "free_mb"));
    TEST_ASSERT_FALSE(has(j, "other_entries"));
    TEST_ASSERT_FALSE(has(j, "fat"));
    cJSON_Delete(j);

    c = card(GW_CARD_UNMOUNTED, "no FAT file system (exFAT, NTFS, ext4, GPT, or no format)");
    snprintf(c.name, sizeof(c.name), "SD64G");
    c.size_mb = 60906;
    gw_card_json(&c, buf, sizeof(buf));
    j = parse(buf);
    TEST_ASSERT_EQUAL_STRING("not_mounted", str(j, "state"));
    TEST_ASSERT_EQUAL_STRING("SD64G", str(j, "name"));
    TEST_ASSERT_EQUAL_DOUBLE(60906, num(j, "size_mb"));
    cJSON_Delete(j);
    gw_card_text(&c, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("NOT MOUNTED: microSD SD64G, 60906 MB, no FAT file system (exFAT, "
                             "NTFS, ext4, GPT, or no format)",
                             buf);

    c = card(GW_CARD_MOUNTED, "");
    snprintf(c.name, sizeof(c.name), "SD32G");
    snprintf(c.fat, sizeof(c.fat), "FAT32");
    c.cluster = 32768;
    c.size_mb = 30436;
    c.free_mb = 30000;
    c.other = 1;
    gw_card_json(&c, buf, sizeof(buf));
    j = parse(buf);
    TEST_ASSERT_EQUAL_STRING("mounted", str(j, "state"));
    TEST_ASSERT_FALSE(has(j, "detail"));
    TEST_ASSERT_EQUAL_STRING("FAT32", str(j, "fat"));
    TEST_ASSERT_EQUAL_DOUBLE(32768, num(j, "cluster"));
    TEST_ASSERT_EQUAL_DOUBLE(30000, num(j, "free_mb"));
    TEST_ASSERT_EQUAL_DOUBLE(1, num(j, "other_entries"));
    cJSON_Delete(j);
    gw_card_text(&c, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("mounted: microSD SD32G, 30436 MB, FAT32, 32768 B clusters, 30000 MB "
                             "free, 1 other entry in the root",
                             buf);
    c.other = 0;
    c.free_mb = 0;   /* a full card: 0 is a value, not "unknown" */
    gw_card_json(&c, buf, sizeof(buf));
    j = parse(buf);
    TEST_ASSERT_EQUAL_DOUBLE(0, num(j, "free_mb"));
    TEST_ASSERT_EQUAL_DOUBLE(0, num(j, "other_entries"));
    cJSON_Delete(j);

    c = card(GW_CARD_UNCHECKED, "store_forward.sd is off: the boot did not look for a card");
    gw_card_text(&c, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("unchecked: store_forward.sd is off: the boot did not look for a card",
                             buf);
    char small[8];
    TEST_ASSERT_EQUAL_size_t(0, gw_card_json(&c, small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("{}", small);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_request_order);
    RUN_TEST(test_simulated);
    RUN_TEST(test_elapsed_and_wrap);
    RUN_TEST(test_format_json);
    RUN_TEST(test_long_reason);
    RUN_TEST(test_answers);
    RUN_TEST(test_card);
    return UNITY_END();
}
