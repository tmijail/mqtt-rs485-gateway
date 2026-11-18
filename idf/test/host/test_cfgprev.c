/* FW-1 (issue #146): the configuration from before an update: the record, the capture at the
 * first change after an update, the decision of a downgrade (same hash, changed, absent) and the
 * JSON paths that differ. */
#include <string.h>

#include "gw_cfgprev.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static uint32_t h(const char *s)
{
    return gw_cfgprev_hash(GW_CFGPREV_HASH_INIT, s, strlen(s));
}

static void test_format_and_parse(void)
{
    gw_cfgprev_t p, back;
    memset(&p, 0, sizeof(p));
    char s[GW_CFGPREV_MAX];
    TEST_ASSERT_FALSE(gw_cfgprev_parse(NULL, &back));
    TEST_ASSERT_FALSE(gw_cfgprev_parse("", &back));
    TEST_ASSERT_FALSE(gw_cfgprev_parse("x=1;a=", &back));
    /* armed only */
    strcpy(p.armed, "v1.0");
    TEST_ASSERT_EQUAL(6, gw_cfgprev_format(&p, s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("a=v1.0", s);
    TEST_ASSERT_TRUE(gw_cfgprev_parse(s, &back));
    TEST_ASSERT_EQUAL_STRING("v1.0", back.armed);
    TEST_ASSERT_EQUAL_STRING("", back.version);
    /* a capture */
    p.armed[0] = '\0';
    strcpy(p.version, "v1;0");   /* a ';' cannot end the version early */
    p.schema = 3;
    p.saved = 1794393600;
    p.hash = 0x0012abcdu;
    gw_cfgprev_format(&p, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("a=;v=v1_0;s=3;t=1794393600;h=0012abcd", s);
    TEST_ASSERT_TRUE(gw_cfgprev_parse(s, &back));
    TEST_ASSERT_EQUAL_STRING("", back.armed);
    TEST_ASSERT_EQUAL_STRING("v1_0", back.version);
    TEST_ASSERT_EQUAL(3, back.schema);
    TEST_ASSERT_EQUAL_INT64(1794393600, back.saved);
    TEST_ASSERT_EQUAL_HEX32(0x0012abcdu, back.hash);
    /* the longest versions fit in the NVS string; a key of a newer image is ignored */
    memset(p.armed, 'a', GW_CFGPREV_VERSION - 1);
    p.armed[GW_CFGPREV_VERSION - 1] = '\0';
    memset(p.version, 'b', GW_CFGPREV_VERSION - 1);
    p.version[GW_CFGPREV_VERSION - 1] = '\0';
    p.saved = 99999999999;
    size_t n = gw_cfgprev_format(&p, s, sizeof(s));
    TEST_ASSERT_LESS_THAN(GW_CFGPREV_MAX, n + 1);
    TEST_ASSERT_TRUE(gw_cfgprev_parse(s, &back));
    TEST_ASSERT_EQUAL_STRING(p.version, back.version);
    TEST_ASSERT_EQUAL_STRING(p.armed, back.armed);
    TEST_ASSERT_TRUE(gw_cfgprev_parse("z=9;a=;v=x;s=1;t=0;h=1", &back));
    TEST_ASSERT_EQUAL_STRING("x", back.version);
    /* a capture without its hash cannot be compared: no capture */
    TEST_ASSERT_FALSE(gw_cfgprev_parse("a=;v=x;s=1;t=0", &back));
}

static void test_hash(void)
{
    TEST_ASSERT_EQUAL_HEX32(GW_CFGPREV_HASH_INIT, h(""));
    TEST_ASSERT_EQUAL_HEX32(0xe40c292cu, h("a"));   /* the FNV-1a 32 test vector */
    /* in pieces: the same value */
    uint32_t x = gw_cfgprev_hash(GW_CFGPREV_HASH_INIT, "{\"a\":", 5);
    x = gw_cfgprev_hash(x, "1}", 2);
    TEST_ASSERT_EQUAL_HEX32(h("{\"a\":1}"), x);
    TEST_ASSERT_NOT_EQUAL(h("{\"a\":1}"), h("{\"a\":2}"));
}

/* armed at the confirmation of an update with the version of the other slot */
static void test_arm(void)
{
    gw_cfgprev_t p;
    memset(&p, 0, sizeof(p));
    TEST_ASSERT_FALSE(gw_cfgprev_arm(&p, "", "v2"));
    TEST_ASSERT_FALSE(gw_cfgprev_arm(&p, NULL, "v2"));
    TEST_ASSERT_FALSE(gw_cfgprev_arm(&p, "v2", "v2"));   /* the same version again */
    TEST_ASSERT_TRUE(gw_cfgprev_arm(&p, "v1", "v2"));
    TEST_ASSERT_EQUAL_STRING("v1", p.armed);
    TEST_ASSERT_FALSE(gw_cfgprev_arm(&p, "v1", "v2"));   /* no change: no NVS write */
    /* arming keeps an earlier capture (for the restore button in recovery mode) */
    strcpy(p.version, "v0");
    p.hash = 7;
    TEST_ASSERT_TRUE(gw_cfgprev_arm(&p, "v3", "v1"));
    TEST_ASSERT_EQUAL_STRING("v0", p.version);
    TEST_ASSERT_EQUAL(7, p.hash);
}

/* the first save that changes the document captures the stored one */
static void test_on_save(void)
{
    gw_cfgprev_t p;
    memset(&p, 0, sizeof(p));
    /* not armed: nothing */
    TEST_ASSERT_EQUAL(GW_CFGPREV_KEEP, gw_cfgprev_on_save(&p, true, 1, h("A"), h("B"), 0));
    TEST_ASSERT_EQUAL_STRING("", p.version);
    gw_cfgprev_arm(&p, "v1", "v2");
    /* the same document again: wait for a change */
    TEST_ASSERT_EQUAL(GW_CFGPREV_KEEP, gw_cfgprev_on_save(&p, true, 1, h("A"), h("A"), 0));
    TEST_ASSERT_EQUAL_STRING("v1", p.armed);
    /* the change: capture with the schema of the stored document and the new hash */
    TEST_ASSERT_EQUAL(GW_CFGPREV_CAPTURE,
                      gw_cfgprev_on_save(&p, true, 1, h("A"), h("B"), 1794393600));
    TEST_ASSERT_EQUAL_STRING("", p.armed);
    TEST_ASSERT_EQUAL_STRING("v1", p.version);
    TEST_ASSERT_EQUAL(1, p.schema);
    TEST_ASSERT_EQUAL_INT64(1794393600, p.saved);
    TEST_ASSERT_EQUAL_HEX32(h("B"), p.hash);
    /* later saves keep the capture (its hash says that the document changed since) */
    TEST_ASSERT_EQUAL(GW_CFGPREV_KEEP, gw_cfgprev_on_save(&p, true, 2, h("B"), h("C"), 0));
    TEST_ASSERT_EQUAL_HEX32(h("B"), p.hash);
    /* no clock: the time is unknown */
    gw_cfgprev_arm(&p, "v2", "v3");
    TEST_ASSERT_EQUAL(GW_CFGPREV_CAPTURE, gw_cfgprev_on_save(&p, true, 2, h("C"), h("D"), 1000));
    TEST_ASSERT_EQUAL_INT64(0, p.saved);
    TEST_ASSERT_EQUAL_STRING("v2", p.version);
    /* armed, and a stored document that this image does not accept (recovery mode, no file):
     * disarmed, the earlier capture stays */
    gw_cfgprev_arm(&p, "v4", "v5");
    TEST_ASSERT_EQUAL(GW_CFGPREV_DISARM, gw_cfgprev_on_save(&p, false, 0, 0, h("E"), 0));
    TEST_ASSERT_EQUAL_STRING("", p.armed);
    TEST_ASSERT_EQUAL_STRING("v2", p.version);
    TEST_ASSERT_EQUAL_HEX32(h("D"), p.hash);
}

/* the decision of a downgrade: same hash, changed, absent */
static void test_decide(void)
{
    gw_cfgprev_t p;
    memset(&p, 0, sizeof(p));
    TEST_ASSERT_EQUAL(GW_CFGPREV_NONE, gw_cfgprev_decide(NULL, "v1", 1, h("B")));
    TEST_ASSERT_EQUAL(GW_CFGPREV_NONE, gw_cfgprev_decide(&p, "v1", 1, h("B")));   /* absent */
    gw_cfgprev_arm(&p, "v1", "v2");
    TEST_ASSERT_EQUAL(GW_CFGPREV_NONE, gw_cfgprev_decide(&p, "v1", 1, h("B")));   /* armed only */
    gw_cfgprev_on_save(&p, true, 1, h("A"), h("B"), 0);
    TEST_ASSERT_EQUAL(GW_CFGPREV_RESTORE, gw_cfgprev_decide(&p, "v1", 1, h("B")));
    TEST_ASSERT_EQUAL(GW_CFGPREV_RESTORE, gw_cfgprev_decide(&p, "v1", 4, h("B")));
    TEST_ASSERT_EQUAL(GW_CFGPREV_CHANGED, gw_cfgprev_decide(&p, "v1", 1, h("C")));
    /* another version: the earlier document is not of it */
    TEST_ASSERT_EQUAL(GW_CFGPREV_NONE, gw_cfgprev_decide(&p, "v0", 1, h("B")));
    TEST_ASSERT_EQUAL(GW_CFGPREV_NONE, gw_cfgprev_decide(&p, "v1.1", 1, h("B")));
    TEST_ASSERT_EQUAL(GW_CFGPREV_NONE, gw_cfgprev_decide(&p, NULL, 1, h("B")));
    /* an image that cannot read the earlier document either */
    TEST_ASSERT_EQUAL(GW_CFGPREV_NONE, gw_cfgprev_decide(&p, "v1", 0, h("B")));
}

static size_t diff(const char *a, const char *b, char *out, size_t cap)
{
    return gw_cfgprev_diff(a, strlen(a), b, strlen(b), out, cap);
}

static void test_diff(void)
{
    char out[160];
    TEST_ASSERT_EQUAL(0, diff("{\"a\":1,\"b\":{\"c\":\"x\"}}", "{\"b\":{\"c\":\"x\"},\"a\":1.0}",
                              out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL(1, diff("{\"a\":1}", "{\"a\":2}", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("a", out);
    /* a key that only one document has, a value of another type, array items */
    const char *a = "{\"mqtt\":{\"host\":\"h1\",\"port\":1883},\"devices\":[{\"name\":\"d\","
                    "\"tags\":[{\"name\":\"t0\"},{\"name\":\"t1\"}]}]}";
    const char *b = "{\"mqtt\":{\"host\":\"h2\",\"port\":\"1883\"},\"devices\":[{\"name\":\"d\","
                    "\"tags\":[{\"name\":\"t0\"},{\"name\":\"T1\"},{\"name\":\"t2\"}]}],"
                    "\"e2e\":{\"new\":true}}";
    TEST_ASSERT_EQUAL(5, diff(a, b, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("mqtt.host, mqtt.port, devices[0].tags[1].name and 2 more", out);
    TEST_ASSERT_EQUAL(1, diff("{\"x\":[1,2]}", "{\"x\":{\"0\":1}}", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("x", out);
    TEST_ASSERT_EQUAL(1, diff("[1]", "{}", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("(the whole document)", out);
    /* a key only in the new document */
    TEST_ASSERT_EQUAL(1, diff("{}", "{\"e2e\":{\"new\":true}}", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("e2e", out);
    /* the keys of the secrets are no difference (NVS holds them): a file written by hand has
     * "password": "", a saved document has none */
    TEST_ASSERT_EQUAL(1, diff("{\"mqtt\":{\"host\":\"h\",\"password\":\"\"},\"web\":{\"password\":"
                              "\"x\"},\"network\":{\"wifi\":{\"password\":\"p\"}}}",
                              "{\"mqtt\":{\"host\":\"h\"},\"web\":{},\"network\":{\"wifi\":{},"
                              "\"hostname\":\"g\"}}", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("network.hostname", out);
    /* a document that does not parse */
    TEST_ASSERT_EQUAL((size_t)-1, diff("{", "{}", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("a document does not parse", out);
    /* a small buffer: cut, still terminated */
    char small[8];
    TEST_ASSERT_EQUAL(5, diff(a, b, small, sizeof(small)));
    TEST_ASSERT_EQUAL(7, strlen(small));
}

/* Issue #226 (fuzz_cfgdoc): a repeated key. The items pair by key and by occurrence, so that a
 * document has no difference from itself (before: the second "" against the first one). */
static void test_diff_repeated_key(void)
{
    char out[160];
    const char *dup = "{\"\":1,\"\":\"\"}";   /* the input that fuzz_cfgdoc found */
    TEST_ASSERT_EQUAL(0, diff(dup, dup, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL(1, diff("{\"a\":1,\"a\":2}", "{\"a\":1,\"a\":3}", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("a", out);
    /* a second "a" that only one document has */
    TEST_ASSERT_EQUAL(1, diff("{\"a\":1}", "{\"a\":1,\"a\":1}", out, sizeof(out)));
    TEST_ASSERT_EQUAL(1, diff("{\"a\":1,\"a\":1}", "{\"a\":1}", out, sizeof(out)));
}

static void test_text(void)
{
    gw_cfgprev_t p;
    memset(&p, 0, sizeof(p));
    char s[120];
    gw_cfgprev_text(&p, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("", s);
    strcpy(p.version, "v1.0");
    gw_cfgprev_text(&p, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("configuration from before the update: v1.0, saved at an unknown "
                             "time", s);
    p.saved = 1794355200 + 10 * 3600 + 20 * 60 + 59;   /* 2026-11-11 10:20:59 UTC */
    gw_cfgprev_text(&p, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("configuration from before the update: v1.0, saved on 2026-11-11 "
                             "10:20 UTC", s);
    p.saved = 1709208000;   /* 2024-02-29 12:00 UTC: a leap day */
    gw_cfgprev_text(&p, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("configuration from before the update: v1.0, saved on 2024-02-29 "
                             "12:00 UTC", s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_format_and_parse);
    RUN_TEST(test_hash);
    RUN_TEST(test_arm);
    RUN_TEST(test_on_save);
    RUN_TEST(test_decide);
    RUN_TEST(test_diff);
    RUN_TEST(test_diff_repeated_key);
    RUN_TEST(test_text);
    return UNITY_END();
}
