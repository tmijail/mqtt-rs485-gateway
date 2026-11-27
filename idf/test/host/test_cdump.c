/* QA-1: issue #132, the core dump's state, summary and base64 lines (gw_cdump); issue #293, the
 * copy of 'coredump dump' with its size, SHA-256 and number of lines, and its stops. */
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

/* ---------- #293: the copy of 'coredump dump' through gw_cdump_copy ---------- */

#define FAKE_DUMP_MAX 70000   /* more than the 64 KB partition */

typedef struct {
    uint8_t dump[FAKE_DUMP_MAX];
    uint32_t size;
    unsigned reads;           /* calls of read */
    uint32_t fail_off;        /* read at this offset fails ... */
    unsigned fail_pass;       /* ... in this pass (1 or 2; 0 = never) */
    unsigned change_pass;     /* from this pass (2), the dump has another byte at offset 100 */
    unsigned pass;            /* the pass of the last read (an offset of 0 starts a pass) */
    char out[FAKE_DUMP_MAX * 2];
    size_t out_len;
    unsigned writes;          /* calls of write */
    unsigned stall_after;     /* write returns false from this call on (0 = never) */
    size_t max_write;         /* the largest text of one write, with '\r' before each '\n' */
    bool partial_write;       /* a write that did not end with '\n' */
} fake_t;

static fake_t s_fake;

static const char *fake_read(void *ctx, uint32_t off, uint8_t *buf, size_t len)
{
    fake_t *f = ctx;
    f->reads++;
    if (off == 0) {
        f->pass++;
    }
    TEST_ASSERT_TRUE(off + len <= f->size);
    if (f->fail_pass == f->pass && off == f->fail_off) {
        return "ESP_ERR_FLASH_OP_FAIL";
    }
    memcpy(buf, f->dump + off, len);
    if (f->change_pass != 0 && f->pass >= f->change_pass && off <= 100 && 100 < off + len) {
        buf[100 - off] ^= 0xffu;
    }
    return NULL;
}

static bool fake_write(void *ctx, const char *text, size_t len)
{
    fake_t *f = ctx;
    f->writes++;
    if (f->stall_after != 0 && f->writes >= f->stall_after) {
        return false;
    }
    size_t crlf = len;
    for (size_t i = 0; i < len; i++) {
        crlf += text[i] == '\n' ? 1u : 0u;
    }
    if (crlf > f->max_write) {
        f->max_write = crlf;
    }
    if (len == 0 || text[len - 1] != '\n') {
        f->partial_write = true;
    }
    TEST_ASSERT_TRUE(f->out_len + len < sizeof(f->out));
    memcpy(f->out + f->out_len, text, len);
    f->out_len += len;
    f->out[f->out_len] = '\0';
    return true;
}

static fake_t *fake_new(uint32_t size)
{
    fake_t *f = &s_fake;
    memset(f, 0, sizeof(*f));
    f->size = size;
    for (uint32_t k = 0; k < size; k++) {
        f->dump[k] = (uint8_t)(k * 31u + (k >> 8) + 7u);
    }
    return f;
}

static gw_cdump_copy_t fake_copy(fake_t *f, gw_cdump_state_t st)
{
    static gw_cdump_work_t w;
    memset(&w, 0x5a, sizeof(w));
    gw_cdump_io_t io = {.ctx = f, .read = fake_read, .write = fake_write};
    gw_cdump_copy_t rc = gw_cdump_copy(&io, st, f->size, &w);
    /* the work memory held bytes of the dump: zeroed at the end */
    static const gw_cdump_work_t zero;
    TEST_ASSERT_EQUAL_MEMORY(&zero, &w, sizeof(w));
    return rc;
}

/* The lines of f->out, NUL-terminated in place; returns their number. */
static size_t split_lines(fake_t *f, char **lines, size_t max)
{
    size_t n = 0;
    char *p = f->out;
    while (*p != '\0' && n < max) {
        char *nl = strchr(p, '\n');
        TEST_ASSERT_NOT_NULL(nl);
        *nl = '\0';
        lines[n++] = p;
        p = nl + 1;
    }
    return n;
}

static void sha_hex(const uint8_t *data, size_t len, char hex[65])
{
    uint8_t h[GW_SHA256_LEN];
    gw_sha256(data, len, h);
    for (size_t i = 0; i < GW_SHA256_LEN; i++) {
        snprintf(hex + 2 * i, 3, "%02x", h[i]);
    }
}

static void test_line_count(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, gw_cdump_line_count(0));
    TEST_ASSERT_EQUAL_UINT32(1, gw_cdump_line_count(1));
    TEST_ASSERT_EQUAL_UINT32(1, gw_cdump_line_count(57));
    TEST_ASSERT_EQUAL_UINT32(2, gw_cdump_line_count(58));
    TEST_ASSERT_EQUAL_UINT32(1150, gw_cdump_line_count(65536));
}

/* A complete copy: the first line (size, state, number of lines, SHA-256), the secrets line, the
 * markers, and between them the base64 lines that decode to the dump. Each write is complete
 * lines and fits into the 256-byte TX buffer of the USB-Serial-JTAG driver with "\r\n". */
static void check_complete_copy(uint32_t size)
{
    fake_t *f = fake_new(size);
    TEST_ASSERT_EQUAL_INT(GW_CDUMP_COPY_OK, fake_copy(f, GW_CDUMP_VALID));
    TEST_ASSERT_FALSE(f->partial_write);
    TEST_ASSERT_TRUE(f->max_write <= 256);
    TEST_ASSERT_EQUAL_UINT(2, f->pass);   /* two reads of the whole dump */

    static char *lines[2000];
    size_t n = split_lines(f, lines, 2000);
    uint32_t want = gw_cdump_line_count(size);
    TEST_ASSERT_EQUAL_size_t(want + 4, n);
    char hex[65];
    sha_hex(f->dump, size, hex);
    char head[200];
    snprintf(head, sizeof(head), "coredump: %lu bytes (valid), %lu lines of base64, sha256 %s",
             (unsigned long)size, (unsigned long)want, hex);
    TEST_ASSERT_EQUAL_STRING(head, lines[0]);
    TEST_ASSERT_EQUAL_STRING("coredump: the dump can contain secrets (passwords, keys): keep the "
                             "copy safe", lines[1]);
    TEST_ASSERT_EQUAL_STRING("-----BEGIN CORE DUMP-----", lines[2]);
    char end[64];
    snprintf(end, sizeof(end), "-----END CORE DUMP----- %lu lines", (unsigned long)want);
    TEST_ASSERT_EQUAL_STRING(end, lines[n - 1]);

    static char joined[FAKE_DUMP_MAX * 2];
    size_t jlen = 0;
    for (size_t k = 3; k < n - 1; k++) {
        size_t len = strlen(lines[k]);
        if (k < n - 2) {
            TEST_ASSERT_EQUAL_size_t(GW_CDUMP_LINE_CHARS, len);
        }
        memcpy(joined + jlen, lines[k], len);
        jlen += len;
    }
    static uint8_t back[FAKE_DUMP_MAX];
    TEST_ASSERT_EQUAL_INT((int)size, gw_base64_decode(joined, jlen, back, sizeof(back)));
    TEST_ASSERT_EQUAL_MEMORY(f->dump, back, size);
}

static void test_copy_complete(void)
{
    static const uint32_t sizes[] = {1, 56, 57, 58, 171, 912, 913, 1000, 12345, 65536};
    for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        check_complete_copy(sizes[k]);
    }
}

/* The state goes into the first line. */
static void test_copy_corrupt_state(void)
{
    fake_t *f = fake_new(100);
    TEST_ASSERT_EQUAL_INT(GW_CDUMP_COPY_OK, fake_copy(f, GW_CDUMP_CORRUPT));
    TEST_ASSERT_EQUAL_INT(0, strncmp(f->out, "coredump: 100 bytes (corrupt), 2 lines of base64, "
                                             "sha256 ", 57));
}

/* A read error in the first pass: no marker, only the INCOMPLETE line. */
static void test_copy_read_error_first(void)
{
    fake_t *f = fake_new(3000);
    f->fail_pass = 1;
    f->fail_off = 912;
    TEST_ASSERT_EQUAL_INT(GW_CDUMP_COPY_READ, fake_copy(f, GW_CDUMP_VALID));
    TEST_ASSERT_EQUAL_STRING("-----CORE DUMP INCOMPLETE: ESP_ERR_FLASH_OP_FAIL-----\n", f->out);
}

/* A read error in the second pass: the lines before it, then INCOMPLETE and no END marker. */
static void test_copy_read_error_second(void)
{
    fake_t *f = fake_new(3000);
    f->fail_pass = 2;
    f->fail_off = 912;
    TEST_ASSERT_EQUAL_INT(GW_CDUMP_COPY_READ, fake_copy(f, GW_CDUMP_VALID));
    TEST_ASSERT_NULL(strstr(f->out, "-----END CORE DUMP-----"));
    static char *lines[100];
    size_t n = split_lines(f, lines, 100);
    TEST_ASSERT_EQUAL_size_t(3 + GW_CDUMP_READ_LINES + 1, n);   /* the first read's 16 lines */
    TEST_ASSERT_EQUAL_STRING("-----CORE DUMP INCOMPLETE: ESP_ERR_FLASH_OP_FAIL-----", lines[n - 1]);
}

/* The bytes of the second read are not the bytes of the first read (an erase during the copy):
 * no END marker. */
static void test_copy_changed(void)
{
    fake_t *f = fake_new(3000);
    f->change_pass = 2;
    TEST_ASSERT_EQUAL_INT(GW_CDUMP_COPY_CHANGED, fake_copy(f, GW_CDUMP_VALID));
    TEST_ASSERT_NULL(strstr(f->out, "-----END CORE DUMP-----"));
    TEST_ASSERT_NOT_NULL(strstr(f->out, "\n-----CORE DUMP INCOMPLETE: the dump changed during "
                                        "the copy-----\n"));
}

/* The console stops taking the text: the copy stops at once, with one more try for the
 * INCOMPLETE line, and gives no END marker. */
static void test_copy_stalled(void)
{
    /* 12345 bytes: 217 lines, 3 + 73 + 1 writes */
    static const unsigned at[] = {1, 2, 3, 4, 10, 76, 77};
    for (size_t k = 0; k < sizeof(at) / sizeof(at[0]); k++) {
        fake_t *f = fake_new(12345);
        f->stall_after = at[k];
        TEST_ASSERT_EQUAL_INT(GW_CDUMP_COPY_STALLED, fake_copy(f, GW_CDUMP_VALID));
        TEST_ASSERT_EQUAL_UINT(at[k] + 1, f->writes);
        TEST_ASSERT_NULL(strstr(f->out, "-----END CORE DUMP-----"));
    }
    /* the END marker itself is not taken */
    fake_t *f = fake_new(100);
    f->stall_after = 5;   /* the first line, secrets, BEGIN, 2 base64 lines (1 write), END */
    TEST_ASSERT_EQUAL_INT(GW_CDUMP_COPY_STALLED, fake_copy(f, GW_CDUMP_VALID));
    TEST_ASSERT_NULL(strstr(f->out, "-----END CORE DUMP-----"));
    TEST_ASSERT_EQUAL_UINT(6, f->writes);
    static char *lines[10];
    TEST_ASSERT_EQUAL_size_t(5, split_lines(f, lines, 10));   /* 3 + the 2 base64 lines */
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
    RUN_TEST(test_line_count);
    RUN_TEST(test_copy_complete);
    RUN_TEST(test_copy_corrupt_state);
    RUN_TEST(test_copy_read_error_first);
    RUN_TEST(test_copy_read_error_second);
    RUN_TEST(test_copy_changed);
    RUN_TEST(test_copy_stalled);
    return UNITY_END();
}
