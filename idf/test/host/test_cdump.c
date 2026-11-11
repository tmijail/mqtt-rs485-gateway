/* QA-1: issue #132, the core dump's state, summary and base64 lines (gw_cdump). */
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "gw_auth.h"
#include "gw_cdump.h"
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

static bool has(const cJSON *j, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(j, key) != NULL;
}

static gw_cdump_info_t sample(void)
{
    gw_cdump_info_t i;
    memset(&i, 0, sizeof(i));
    i.state = GW_CDUMP_VALID;
    i.size = 12345;
    i.has_summary = true;
    snprintf(i.task, sizeof(i.task), "panic");
    i.pc = 0x4037a1b2;
    i.depth = 3;
    i.bt[0] = 0x4037a1b2;
    i.bt[1] = 0x42001234;
    i.bt[2] = 0x4200abcd;
    i.exc_cause = 28;
    i.exc_vaddr = 0x10;
    snprintf(i.reason, sizeof(i.reason), "abort() was called at PC 0x42001234 on core 0 \"x\\\n");
    snprintf(i.app_sha256, sizeof(i.app_sha256), "0123abcd");
    return i;
}

/* RFC 4648 section 10 test vectors. */
static void test_b64_vectors(void)
{
    static const char *const in[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    static const char *const want[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=",
                                       "Zm9vYmFy"};
    char out[16];
    for (size_t k = 0; k < sizeof(in) / sizeof(in[0]); k++) {
        size_t n = gw_cdump_b64((const uint8_t *)in[k], strlen(in[k]), out, sizeof(out));
        TEST_ASSERT_EQUAL_STRING(want[k], out);
        TEST_ASSERT_EQUAL_size_t(strlen(want[k]), n);
    }
}

/* Exactly 4 * ceil(len / 3) + 1 bytes are needed; one less is refused, with an empty string. */
static void test_b64_capacity(void)
{
    const uint8_t data[4] = {1, 2, 3, 4};
    char out[9];
    TEST_ASSERT_EQUAL_size_t(8, gw_cdump_b64(data, 4, out, 9));
    memset(out, 'x', sizeof(out));
    TEST_ASSERT_EQUAL_size_t(0, gw_cdump_b64(data, 4, out, 8));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL_size_t(0, gw_cdump_b64(data, 4, out, 0));
}

/* The lines of 'coredump dump' (57 bytes each, the last one shorter) decode, line by line as
 * esp-coredump does and also joined, to the bytes of the dump; each line is 76 characters. */
static void test_b64_lines_round_trip(void)
{
    static uint8_t dump[1000];
    for (size_t k = 0; k < sizeof(dump); k++) {
        dump[k] = (uint8_t)(k * 7 + 3);
    }
    static char joined[2000];
    size_t jlen = 0;
    static uint8_t back[1000];
    size_t blen = 0;
    for (size_t off = 0; off < sizeof(dump); off += GW_CDUMP_LINE_BYTES) {
        size_t n = sizeof(dump) - off < GW_CDUMP_LINE_BYTES ? sizeof(dump) - off
                                                            : GW_CDUMP_LINE_BYTES;
        char line[GW_CDUMP_LINE_CHARS + 1];
        size_t len = gw_cdump_b64(dump + off, n, line, sizeof(line));
        TEST_ASSERT_TRUE(len > 0);
        if (n == GW_CDUMP_LINE_BYTES) {
            TEST_ASSERT_EQUAL_size_t(GW_CDUMP_LINE_CHARS, len);
            TEST_ASSERT_NULL(strchr(line, '='));   /* no padding inside the text */
        }
        int d = gw_base64_decode(line, len, back + blen, sizeof(back) - blen);
        TEST_ASSERT_EQUAL_INT((int)n, d);
        blen += (size_t)d;
        memcpy(joined + jlen, line, len);
        jlen += len;
    }
    TEST_ASSERT_EQUAL_size_t(sizeof(dump), blen);
    TEST_ASSERT_EQUAL_MEMORY(dump, back, sizeof(dump));
    static uint8_t all[1000];
    TEST_ASSERT_EQUAL_INT((int)sizeof(dump), gw_base64_decode(joined, jlen, all, sizeof(all)));
    TEST_ASSERT_EQUAL_MEMORY(dump, all, sizeof(dump));
}

static void test_status_json(void)
{
    char out[128];
    TEST_ASSERT_TRUE(gw_cdump_status_json(GW_CDUMP_NONE, 0, out, sizeof(out)) > 0);
    TEST_ASSERT_EQUAL_STRING("{\"present\":false,\"state\":\"none\",\"size\":0}", out);
    gw_cdump_status_json(GW_CDUMP_VALID, 12345, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("{\"present\":true,\"state\":\"valid\",\"size\":12345}", out);
    gw_cdump_status_json(GW_CDUMP_CORRUPT, 0, out, sizeof(out));   /* size not readable */
    TEST_ASSERT_EQUAL_STRING("{\"present\":true,\"state\":\"corrupt\",\"size\":0}", out);
    /* too small: "{}" and 0 */
    TEST_ASSERT_EQUAL_size_t(0, gw_cdump_status_json(GW_CDUMP_CORRUPT, 12345, out, 20));
    TEST_ASSERT_EQUAL_STRING("{}", out);
}

static void test_summary_json(void)
{
    char out[1024];
    gw_cdump_info_t i = sample();
    TEST_ASSERT_TRUE(gw_cdump_summary_json(&i, "panic", out, sizeof(out)) > 0);
    cJSON *j = parse(out);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(j, "present")));
    TEST_ASSERT_EQUAL_STRING("valid", str(j, "state"));
    TEST_ASSERT_EQUAL_STRING("panic", str(j, "reset_reason"));
    TEST_ASSERT_EQUAL_STRING("panic", str(j, "task"));
    TEST_ASSERT_EQUAL_STRING("0x4037a1b2", str(j, "pc"));
    TEST_ASSERT_EQUAL_STRING("0x00000010", str(j, "exc_vaddr"));
    TEST_ASSERT_EQUAL_STRING("abort() was called at PC 0x42001234 on core 0 \"x\\\n",
                             str(j, "reason"));   /* escaped, and back */
    TEST_ASSERT_EQUAL_STRING("0123abcd", str(j, "app_elf_sha256"));
    const cJSON *bt = cJSON_GetObjectItem(j, "backtrace");
    TEST_ASSERT_EQUAL_INT(3, cJSON_GetArraySize(bt));
    TEST_ASSERT_EQUAL_STRING("0x4200abcd", cJSON_GetArrayItem(bt, 2)->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(j, "backtrace_corrupted")));
    TEST_ASSERT_EQUAL_DOUBLE(28, cJSON_GetObjectItem(j, "exc_cause")->valuedouble);
    cJSON_Delete(j);

    /* a corrupt dump, or one whose summary was not read: the status fields only */
    i.state = GW_CDUMP_CORRUPT;
    gw_cdump_summary_json(&i, "panic", out, sizeof(out));
    j = parse(out);
    TEST_ASSERT_EQUAL_STRING("corrupt", str(j, "state"));
    TEST_ASSERT_FALSE(has(j, "task"));
    TEST_ASSERT_FALSE(has(j, "backtrace"));
    cJSON_Delete(j);
    memset(&i, 0, sizeof(i));
    gw_cdump_summary_json(&i, NULL, out, sizeof(out));
    j = parse(out);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(j, "present")));
    TEST_ASSERT_EQUAL_STRING("", str(j, "reset_reason"));
    cJSON_Delete(j);

    /* a depth past the array is cut to it; too small a buffer gives "{}" */
    i = sample();
    i.depth = 200;
    TEST_ASSERT_TRUE(gw_cdump_summary_json(&i, "panic", out, sizeof(out)) > 0);
    j = parse(out);
    TEST_ASSERT_EQUAL_INT(GW_CDUMP_BT_MAX, cJSON_GetArraySize(cJSON_GetObjectItem(j, "backtrace")));
    cJSON_Delete(j);
    TEST_ASSERT_EQUAL_size_t(0, gw_cdump_summary_json(&i, "panic", out, 64));
    TEST_ASSERT_EQUAL_STRING("{}", out);
}

static void test_text(void)
{
    char out[512];
    gw_cdump_info_t i;
    memset(&i, 0, sizeof(i));
    gw_cdump_text(&i, "power-on", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("coredump  none in flash\n", out);
    i = sample();
    size_t n = gw_cdump_text(&i, "panic", out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(strlen(out), n);
    TEST_ASSERT_NOT_NULL(strstr(out, "coredump  12345 bytes in flash, checksum correct; the reset "
                                     "reason of this boot: panic\n"));
    TEST_ASSERT_NOT_NULL(strstr(out, "task      panic\n"));
    TEST_ASSERT_NOT_NULL(strstr(out, "pc        0x4037a1b2\n"));
    TEST_ASSERT_NOT_NULL(strstr(out, "backtrace 0x4037a1b2 0x42001234 0x4200abcd\n"));
    /* the reason on one line: its newline becomes a space */
    TEST_ASSERT_NOT_NULL(strstr(out, "reason    abort() was called at PC 0x42001234 on core 0 "
                                     "\"x\\ \n"));
    i.bt_corrupted = true;
    gw_cdump_text(&i, "panic", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "0x4200abcd |<-CORRUPTED\n"));
    i.state = GW_CDUMP_CORRUPT;
    gw_cdump_text(&i, "panic", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "CORRUPT"));
    TEST_ASSERT_NULL(strstr(out, "task"));
    /* truncated: what fits, ended by a newline */
    i = sample();
    n = gw_cdump_text(&i, "panic", out, 40);
    TEST_ASSERT_EQUAL_size_t(39, n);
    TEST_ASSERT_EQUAL_INT('\n', out[38]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_b64_vectors);
    RUN_TEST(test_b64_capacity);
    RUN_TEST(test_b64_lines_round_trip);
    RUN_TEST(test_status_json);
    RUN_TEST(test_summary_json);
    RUN_TEST(test_text);
    return UNITY_END();
}
