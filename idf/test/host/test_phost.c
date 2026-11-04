/* QA-1: Sparkplug 3.0 Primary Host Application STATE payloads and the timestamp rule (SPB-13). */
#include <string.h>

#include "gw_phost.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static bool parse(const char *json, bool *online, uint64_t *ts)
{
    return gw_phost_parse((const uint8_t *)json, strlen(json), online, ts);
}

static gw_phost_result_t state(gw_phost_t *p, const char *json)
{
    return gw_phost_on_state(p, (const uint8_t *)json, strlen(json));
}

static void test_parse_valid(void)
{
    bool on = false;
    uint64_t ts = 0;
    TEST_ASSERT_TRUE(parse("{\"online\":true,\"timestamp\":1700000000123}", &on, &ts));
    TEST_ASSERT_TRUE(on);
    TEST_ASSERT_EQUAL_UINT64(1700000000123ull, ts);
    /* key order, whitespace and unknown keys (of any JSON type) do not matter */
    TEST_ASSERT_TRUE(parse(" {\n \"timestamp\" : 5 , \"x\":{\"a\":[1,\"]\",null]}, \"online\" : false } ",
                           &on, &ts));
    TEST_ASSERT_FALSE(on);
    TEST_ASSERT_EQUAL_UINT64(5, ts);
    TEST_ASSERT_TRUE(parse("{\"online\":true,\"timestamp\":18446744073709551615}", &on, &ts));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, ts);
}

static void test_parse_invalid(void)
{
    static const char *const bad[] = {
        "",
        "ONLINE",                                            /* Sparkplug 2.x payload */
        "{\"online\":true}",                                 /* no timestamp */
        "{\"timestamp\":1}",                                 /* no online */
        "{\"online\":\"true\",\"timestamp\":1}",             /* string, not Boolean */
        "{\"online\":1,\"timestamp\":1}",
        "{\"online\":true,\"timestamp\":-1}",
        "{\"online\":true,\"timestamp\":1.5}",
        "{\"online\":true,\"timestamp\":1e3}",
        "{\"online\":true,\"timestamp\":\"1\"}",
        "{\"online\":true,\"timestamp\":18446744073709551616}", /* overflow */
        "{\"online\":true,\"online\":false,\"timestamp\":1}",   /* duplicated key */
        "{\"online\":true,\"timestamp\":1",                  /* truncated */
        "{\"online\":true,\"timestamp\":1} x",               /* trailing content */
        "{\"online\":true,\"timestamp\":1,}",
        "[{\"online\":true,\"timestamp\":1}]",
        "{\"online\":true,\"timestamp\":1,\"x\":[[[[[[1]]]]]]}", /* nesting beyond the bound */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        bool on;
        uint64_t ts;
        TEST_ASSERT_FALSE_MESSAGE(parse(bad[i], &on, &ts), bad[i]);
    }
    /* an embedded NUL is not a number character */
    static const uint8_t nul[] = "{\"x\":1\0,\"online\":true,\"timestamp\":1}";
    bool on;
    uint64_t ts;
    TEST_ASSERT_FALSE(gw_phost_parse(nul, sizeof(nul) - 1, &on, &ts));
}

/* Older STATE messages are ignored; the same timestamp again (the retained message of a new
 * session) is accepted; online is per session. */
static void test_timestamps_and_sessions(void)
{
    gw_phost_t p;
    gw_phost_init(&p);
    TEST_ASSERT_FALSE(p.online);
    TEST_ASSERT_EQUAL(GW_PHOST_OFFLINE, state(&p, "{\"online\":false,\"timestamp\":100}"));
    TEST_ASSERT_EQUAL(GW_PHOST_ONLINE, state(&p, "{\"online\":true,\"timestamp\":200}"));
    TEST_ASSERT_TRUE(p.online);
    /* a stale offline (an old retained message delivered late) does not take the node down */
    TEST_ASSERT_EQUAL(GW_PHOST_STALE, state(&p, "{\"online\":false,\"timestamp\":150}"));
    TEST_ASSERT_TRUE(p.online);
    TEST_ASSERT_EQUAL(GW_PHOST_INVALID, state(&p, "OFFLINE"));
    TEST_ASSERT_TRUE(p.online);
    TEST_ASSERT_EQUAL(GW_PHOST_OFFLINE, state(&p, "{\"online\":false,\"timestamp\":300}"));
    TEST_ASSERT_FALSE(p.online);
    /* new session: unknown until the retained message arrives, with the same timestamp */
    gw_phost_session_start(&p);
    TEST_ASSERT_EQUAL(GW_PHOST_OFFLINE, state(&p, "{\"online\":false,\"timestamp\":300}"));
    TEST_ASSERT_EQUAL(GW_PHOST_ONLINE, state(&p, "{\"online\":true,\"timestamp\":301}"));
    gw_phost_session_start(&p);
    TEST_ASSERT_FALSE(p.online);
    TEST_ASSERT_EQUAL(GW_PHOST_ONLINE, state(&p, "{\"online\":true,\"timestamp\":301}"));
    TEST_ASSERT_TRUE(p.online);
    TEST_ASSERT_EQUAL_UINT64(301, p.last_ts);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_valid);
    RUN_TEST(test_parse_invalid);
    RUN_TEST(test_timestamps_and_sessions);
    return UNITY_END();
}
