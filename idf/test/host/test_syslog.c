/* QA-1: remote syslog (REL-4): esp_log line parsing, RFC 5424 formatting, bounded ring. */
#include <stdio.h>
#include <string.h>

#include "gw_syslog.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static gw_syslog_rec_t rec;

static void test_format_severity(void)
{
    TEST_ASSERT_EQUAL_INT(3, gw_syslog_format_severity("E (%lu) %s: x\n"));
    TEST_ASSERT_EQUAL_INT(4, gw_syslog_format_severity("W (%lu) %s: x\n"));
    TEST_ASSERT_EQUAL_INT(6, gw_syslog_format_severity("I (%lu) %s: x\n"));
    TEST_ASSERT_EQUAL_INT(7, gw_syslog_format_severity("D (%lu) %s: x\n"));
    TEST_ASSERT_EQUAL_INT(7, gw_syslog_format_severity("V (%lu) %s: x\n"));
    TEST_ASSERT_EQUAL_INT(4, gw_syslog_format_severity("\x1b[0;33mW (%lu) %s: x\x1b[0m\n"));
    TEST_ASSERT_EQUAL_INT(-1, gw_syslog_format_severity("%s"));
    TEST_ASSERT_EQUAL_INT(-1, gw_syslog_format_severity(""));
}

static void test_parse_esp_log_lines(void)
{
    TEST_ASSERT_TRUE(gw_syslog_parse_line("I (1903) edge: Sparkplug session up (bdSeq=3)\n", &rec));
    TEST_ASSERT_EQUAL(6, rec.severity);
    TEST_ASSERT_EQUAL_STRING("edge", rec.tag);
    TEST_ASSERT_EQUAL_STRING("Sparkplug session up (bdSeq=3)", rec.msg);

    /* colours, system-time timestamps and ": " inside the message */
    TEST_ASSERT_TRUE(gw_syslog_parse_line(
        "\x1b[0;31mE (12:34:56.789) main: configuration rejected: a: b\x1b[0m\n", &rec));
    TEST_ASSERT_EQUAL(3, rec.severity);
    TEST_ASSERT_EQUAL_STRING("main", rec.tag);
    TEST_ASSERT_EQUAL_STRING("configuration rejected: a: b", rec.msg);

    TEST_ASSERT_TRUE(gw_syslog_parse_line("W (5) esp-tls: x\r\n", &rec));
    TEST_ASSERT_EQUAL(4, rec.severity);
    TEST_ASSERT_EQUAL_STRING("esp-tls", rec.tag);
    TEST_ASSERT_EQUAL_STRING("x", rec.msg);

    /* not esp_log-shaped: kept whole as info */
    TEST_ASSERT_FALSE(gw_syslog_parse_line("free text without a prefix\n", &rec));
    TEST_ASSERT_EQUAL(6, rec.severity);
    TEST_ASSERT_EQUAL_STRING("-", rec.tag);
    TEST_ASSERT_EQUAL_STRING("free text without a prefix", rec.msg);
    TEST_ASSERT_FALSE(gw_syslog_parse_line("I (12 no closing", &rec));
    TEST_ASSERT_FALSE(gw_syslog_parse_line("", &rec));
    TEST_ASSERT_EQUAL_STRING("", rec.msg);

    /* long tag and message are truncated, never overflow */
    char line[600];
    memset(line, 'x', sizeof(line));
    memcpy(line, "I (1) ", 6);
    memcpy(line + 100, ": ", 2);
    line[sizeof(line) - 1] = '\0';
    TEST_ASSERT_TRUE(gw_syslog_parse_line(line, &rec));
    TEST_ASSERT_EQUAL(GW_SYSLOG_TAG_MAX - 1, strlen(rec.tag));
    TEST_ASSERT_EQUAL(GW_SYSLOG_MSG_MAX - 1, strlen(rec.msg));
}

static void test_timestamp(void)
{
    char ts[25];
    gw_syslog_timestamp(0, ts);
    TEST_ASSERT_EQUAL_STRING("1970-01-01T00:00:00.000Z", ts);
    gw_syslog_timestamp(1793708096789ULL, ts);   /* 2026-11-03T12:14:56.789Z */
    TEST_ASSERT_EQUAL_STRING("2026-11-03T12:14:56.789Z", ts);
    gw_syslog_timestamp(951782400000ULL, ts);    /* leap day 2000 */
    TEST_ASSERT_EQUAL_STRING("2000-02-29T00:00:00.000Z", ts);
    gw_syslog_timestamp(4102444799999ULL, ts);
    TEST_ASSERT_EQUAL_STRING("2099-12-31T23:59:59.999Z", ts);
}

static void test_rfc5424_format(void)
{
    char out[300];
    gw_syslog_rec_t r = {.time_ms = 1793708096789ULL, .severity = 6, .tag = "edge",
                         .msg = "Sparkplug session up (bdSeq=3)"};
    size_t n = gw_syslog_format(&r, "GW01", 7, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("<134>1 2026-11-03T12:14:56.789Z GW01 edge - - [meta sequenceId=\"7\"] "
                             "Sparkplug session up (bdSeq=3)",
                             out);
    TEST_ASSERT_EQUAL(strlen(out), n);

    /* clock not set: NILVALUE timestamp; hostname/app-name sanitised; control chars in MSG */
    gw_syslog_rec_t w = {.time_ms = 0, .severity = 4, .tag = "my tag", .msg = "a\tb\nc"};
    gw_syslog_format(&w, "Plant 1/\xc3\xa1", 1, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("<132>1 - Plant_1/__ my_tag - - [meta sequenceId=\"1\"] a b c", out);
    gw_syslog_rec_t e = {.severity = 3, .tag = "", .msg = ""};
    gw_syslog_format(&e, "", 2, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("<131>1 - - - - - [meta sequenceId=\"2\"] ", out);

    /* truncation */
    char small[20];
    n = gw_syslog_format(&r, "GW01", 7, small, sizeof(small));
    TEST_ASSERT_EQUAL(19, n);
    TEST_ASSERT_EQUAL_STRING("<134>1 2026-11-03T1", small);
}

static void test_ring_bounded_and_drops(void)
{
    gw_syslog_rec_t slots[4];
    gw_syslog_ring_t r;
    gw_syslog_ring_init(&r, slots, 4);   /* capacity 3 */
    TEST_ASSERT_NULL(gw_syslog_ring_peek(&r));
    for (int i = 0; i < 5; i++) {
        gw_syslog_rec_t *s = gw_syslog_ring_reserve(&r);
        if (i < 3) {
            TEST_ASSERT_NOT_NULL(s);
            snprintf(s->msg, sizeof(s->msg), "m%d", i);
            gw_syslog_ring_commit(&r);
        } else {
            TEST_ASSERT_NULL(s);   /* full: the producer never waits */
        }
    }
    gw_syslog_ring_count_drop(&r);
    TEST_ASSERT_EQUAL_UINT(3, gw_syslog_ring_take_dropped(&r));
    TEST_ASSERT_EQUAL_UINT(0, gw_syslog_ring_take_dropped(&r));
    for (int i = 0; i < 3; i++) {
        const gw_syslog_rec_t *s = gw_syslog_ring_peek(&r);
        char want[8];
        snprintf(want, sizeof(want), "m%d", i);
        TEST_ASSERT_NOT_NULL(s);
        TEST_ASSERT_EQUAL_STRING(want, s->msg);
        gw_syslog_ring_pop(&r);
    }
    TEST_ASSERT_NULL(gw_syslog_ring_peek(&r));
    /* wrap-around keeps FIFO order */
    for (int round = 0; round < 10; round++) {
        gw_syslog_rec_t *s = gw_syslog_ring_reserve(&r);
        TEST_ASSERT_NOT_NULL(s);
        s->severity = (uint8_t)round;
        gw_syslog_ring_commit(&r);
        TEST_ASSERT_EQUAL(round, gw_syslog_ring_peek(&r)->severity);
        gw_syslog_ring_pop(&r);
    }
    /* reserve without commit publishes nothing */
    TEST_ASSERT_NOT_NULL(gw_syslog_ring_reserve(&r));
    TEST_ASSERT_NULL(gw_syslog_ring_peek(&r));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_format_severity);
    RUN_TEST(test_parse_esp_log_lines);
    RUN_TEST(test_timestamp);
    RUN_TEST(test_rfc5424_format);
    RUN_TEST(test_ring_bounded_and_drops);
    return UNITY_END();
}
