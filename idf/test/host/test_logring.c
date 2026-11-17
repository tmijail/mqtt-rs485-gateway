/* Issues #200 and #199: the ring of the live log, the tag of a line and the table of the log
 * levels. */
#include <stdio.h>
#include <string.h>

#include "gw_logring.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void put(gw_logring_t *r, const char *s)
{
    gw_logring_put(r, s, strlen(s));
}

static void test_lines_and_numbers(void)
{
    char buf[256], out[256];
    gw_logring_t r;
    gw_logring_init(&r, buf, sizeof(buf));
    uint32_t from, last;
    TEST_ASSERT_EQUAL_UINT32(1, gw_logring_first(&r));
    TEST_ASSERT_EQUAL(0, gw_logring_copy(&r, 0, out, sizeof(out), &from, &last));
    TEST_ASSERT_EQUAL_UINT32(0, last);
    put(&r, "I (10) net: link up\n");
    put(&r, "W (20) edge: MQTT down\r\n");
    put(&r, "\n");   /* empty: not kept, no number */
    TEST_ASSERT_EQUAL_UINT32(2, r.lines);
    size_t n = gw_logring_copy(&r, 0, out, sizeof(out), &from, &last);
    TEST_ASSERT_EQUAL_STRING_LEN("I (10) net: link up\nW (20) edge: MQTT down\n", out, n);
    TEST_ASSERT_EQUAL_UINT32(1, from);
    TEST_ASSERT_EQUAL_UINT32(2, last);
    /* only the lines after the last number that the reader has */
    n = gw_logring_copy(&r, 1, out, sizeof(out), &from, &last);
    TEST_ASSERT_EQUAL_STRING_LEN("W (20) edge: MQTT down\n", out, n);
    TEST_ASSERT_EQUAL_UINT32(2, from);
    TEST_ASSERT_EQUAL(0, gw_logring_copy(&r, 2, out, sizeof(out), &from, &last));
    TEST_ASSERT_EQUAL_UINT32(2, from);
    TEST_ASSERT_EQUAL_UINT32(2, last);
}

/* ANSI colors go, control characters become spaces, a long line is cut */
static void test_clean_and_cut(void)
{
    char buf[1024], out[1024];
    gw_logring_t r;
    gw_logring_init(&r, buf, sizeof(buf));
    put(&r, "\x1b[0;33mW (5) mb: unit\t3 timeout\x1b[0m\n");
    char big[400];
    memset(big, 'x', sizeof(big));
    gw_logring_put(&r, big, sizeof(big));
    uint32_t from, last;
    size_t n = gw_logring_copy(&r, 0, out, sizeof(out), &from, &last);
    TEST_ASSERT_EQUAL_STRING_LEN("W (5) mb: unit 3 timeout\n", out, 25);
    TEST_ASSERT_EQUAL(25 + GW_LOGRING_LINE_MAX + 1, n);
    TEST_ASSERT_EQUAL('\n', out[n - 1]);
}

/* a full buffer drops its oldest lines; the numbers go on, and the reader can tell how many
 * lines it lost (from - since - 1) */
static void test_wrap_drops_oldest(void)
{
    char buf[100], out[200];
    gw_logring_t r;
    gw_logring_init(&r, buf, sizeof(buf));
    char line[32];
    for (int i = 1; i <= 40; i++) {
        snprintf(line, sizeof(line), "I (%d) t: line %02d", i, i);   /* 17 to 18 bytes + '\n' */
        put(&r, line);
        TEST_ASSERT_TRUE(r.used <= sizeof(buf));
    }
    TEST_ASSERT_EQUAL_UINT32(41, r.next);
    uint32_t from, last;
    size_t n = gw_logring_copy(&r, 10, out, sizeof(out), &from, &last);
    TEST_ASSERT_EQUAL_UINT32(gw_logring_first(&r), from);
    TEST_ASSERT_EQUAL_UINT32(40, last);
    TEST_ASSERT_EQUAL_UINT32(r.lines, last - from + 1);
    TEST_ASSERT_EQUAL(r.used, n);
    TEST_ASSERT_EQUAL_STRING_LEN("I (40) t: line 40\n", out + n - 18, 18);
    /* the last whole lines only, up to the room of the reader */
    n = gw_logring_copy(&r, 0, out, 30, &from, &last);
    TEST_ASSERT_TRUE(n <= 30);
    TEST_ASSERT_EQUAL('\n', out[n - 1]);
    TEST_ASSERT_EQUAL_UINT32(gw_logring_first(&r), from);
}

static void test_tag_of_a_line(void)
{
    const char *t;
    size_t n;
    TEST_ASSERT_TRUE(gw_logring_tag("I (1234) edge: up", 17, &t, &n));
    TEST_ASSERT_EQUAL_STRING_LEN("edge", t, n);
    const char *c = "\x1b[0;31mE (77) esp-tls-mbedtls: handshake\x1b[0m";
    TEST_ASSERT_TRUE(gw_logring_tag(c, strlen(c), &t, &n));
    TEST_ASSERT_EQUAL_STRING_LEN("esp-tls-mbedtls", t, n);
    TEST_ASSERT_FALSE(gw_logring_tag("hello: world", 12, &t, &n));
    TEST_ASSERT_FALSE(gw_logring_tag("I (12) no colon", 15, &t, &n));
}

static void test_levels(void)
{
    TEST_ASSERT_EQUAL_STRING("debug", gw_loglevel_name(GW_LOGLV_DEBUG));
    TEST_ASSERT_EQUAL_STRING("?", gw_loglevel_name(9));
    TEST_ASSERT_EQUAL(GW_LOGLV_VERBOSE, gw_loglevel_parse("verbose"));
    TEST_ASSERT_EQUAL(GW_LOGLV_NONE, gw_loglevel_parse("none"));
    TEST_ASSERT_EQUAL(-1, gw_loglevel_parse("warning"));
}

static void test_tag_table(void)
{
    static gw_logtags_t t;
    gw_logtags_init(&t);
    TEST_ASSERT_NOT_NULL(gw_logtags_add(&t, "poll", 4));
    TEST_ASSERT_NOT_NULL(gw_logtags_add(&t, "mbx", 2));   /* "mb" */
    TEST_ASSERT_EQUAL(2, t.count);
    TEST_ASSERT_EQUAL_PTR(&t.tag[0], gw_logtags_add(&t, "poll", 4));   /* no copy */
    TEST_ASSERT_NULL(gw_logtags_add(&t, "", 0));
    TEST_ASSERT_NULL(gw_logtags_add(&t, "a-tag-that-is-much-too-long", 27));
    const gw_logtag_t *ch[8];
    TEST_ASSERT_EQUAL(0, gw_logtags_changed(&t, ch, 8));
    TEST_ASSERT_TRUE(gw_logtags_set(&t, "poll", GW_LOGLV_DEBUG, GW_LOGLV_INFO));
    TEST_ASSERT_TRUE(gw_logtags_set(&t, "httpd", GW_LOGLV_VERBOSE, GW_LOGLV_INFO));   /* new */
    TEST_ASSERT_TRUE(gw_logtags_set(&t, "*", GW_LOGLV_WARN, GW_LOGLV_INFO));
    TEST_ASSERT_EQUAL(3, gw_logtags_changed(&t, ch, 8));
    TEST_ASSERT_EQUAL_STRING("poll", ch[0]->name);
    TEST_ASSERT_EQUAL(GW_LOGLV_DEBUG, ch[0]->level);
    TEST_ASSERT_EQUAL_STRING("*", ch[2]->name);
    /* the default level again: no more a change */
    TEST_ASSERT_TRUE(gw_logtags_set(&t, "poll", GW_LOGLV_INFO, GW_LOGLV_INFO));
    TEST_ASSERT_EQUAL(2, gw_logtags_changed(&t, ch, 8));
    TEST_ASSERT_EQUAL_STRING("httpd", ch[0]->name);
    gw_logtags_reset_all(&t);
    TEST_ASSERT_EQUAL(0, gw_logtags_changed(&t, ch, 8));
    TEST_ASSERT_EQUAL(4, t.count);   /* the tags stay in the list */
    /* bounded: a full table keeps its tags and refuses a new one */
    char name[8];
    for (int i = 0; i < GW_LOGTAG_MAX; i++) {
        snprintf(name, sizeof(name), "t%d", i);
        gw_logtags_add(&t, name, strlen(name));
    }
    TEST_ASSERT_EQUAL(GW_LOGTAG_MAX, t.count);
    TEST_ASSERT_FALSE(gw_logtags_set(&t, "newtag", GW_LOGLV_DEBUG, GW_LOGLV_INFO));
    TEST_ASSERT_TRUE(gw_logtags_set(&t, "poll", GW_LOGLV_DEBUG, GW_LOGLV_INFO));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_lines_and_numbers);
    RUN_TEST(test_clean_and_cut);
    RUN_TEST(test_wrap_drops_oldest);
    RUN_TEST(test_tag_of_a_line);
    RUN_TEST(test_levels);
    RUN_TEST(test_tag_table);
    return UNITY_END();
}
