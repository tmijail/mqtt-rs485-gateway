/* Issue #182: the Modbus test read of the REST API (POST /api/modbus/read): request and answer. */
#include <stdio.h>
#include <string.h>

#include "gw_mbdiag.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static gw_mbdiag_req_t rq;
static char why[160];
static char out[640];

static bool parse(const char *json)
{
    why[0] = '\0';
    return gw_mbdiag_parse(json, strlen(json), &rq, why, sizeof(why));
}

#define ASSERT_REFUSED(json, fragment)                                        \
    do {                                                                      \
        TEST_ASSERT_FALSE_MESSAGE(parse(json), json);                         \
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(why, fragment), why);             \
    } while (0)

static void test_plain_reads(void)
{
    TEST_ASSERT_TRUE_MESSAGE(parse("{\"unit\":1,\"area\":\"holding\",\"address\":10}"), why);
    TEST_ASSERT_EQUAL(1, rq.unit);
    TEST_ASSERT_EQUAL(GW_AREA_HOLDING, rq.area);
    TEST_ASSERT_EQUAL(10, rq.address);
    TEST_ASSERT_EQUAL(1, rq.count);
    TEST_ASSERT_FALSE(rq.convert);
    TEST_ASSERT_EQUAL(MB_FC_READ_HOLDING_REGISTERS, gw_mbdiag_fc(&rq));
    TEST_ASSERT_TRUE_MESSAGE(parse("{\"unit\":247,\"area\":\"coil\",\"address\":65520,\"count\":16}"), why);
    TEST_ASSERT_EQUAL(MB_FC_READ_COILS, gw_mbdiag_fc(&rq));
    TEST_ASSERT_EQUAL(16, rq.count);
    TEST_ASSERT_TRUE(parse("{\"unit\":2,\"area\":\"discrete\",\"address\":0}"));
    TEST_ASSERT_EQUAL(MB_FC_READ_DISCRETE_INPUTS, gw_mbdiag_fc(&rq));
    TEST_ASSERT_TRUE(parse("{\"unit\":2,\"area\":\"input\",\"address\":0,\"count\":2}"));
    TEST_ASSERT_EQUAL(MB_FC_READ_INPUT_REGISTERS, gw_mbdiag_fc(&rq));
}

static void test_typed_reads(void)
{
    TEST_ASSERT_TRUE_MESSAGE(
        parse("{\"unit\":1,\"area\":\"input\",\"address\":0,\"type\":\"float32\"}"), why);
    TEST_ASSERT_TRUE(rq.convert);
    TEST_ASSERT_EQUAL(2, rq.count);   /* the width of the type */
    TEST_ASSERT_EQUAL(GW_TYPE_FLOAT32, rq.tag.type);
    TEST_ASSERT_EQUAL(GW_WORD_BIG, rq.tag.word_order);
    TEST_ASSERT_TRUE(parse("{\"unit\":1,\"area\":\"holding\",\"address\":34,\"type\":\"int64\","
                           "\"word_order\":\"CDAB\",\"count\":6}"));
    TEST_ASSERT_EQUAL(6, rq.count);
    TEST_ASSERT_EQUAL(GW_WORD_LITTLE, rq.tag.word_order);
    TEST_ASSERT_TRUE(parse("{\"unit\":1,\"area\":\"holding\",\"address\":50,\"type\":\"string\","
                           "\"count\":8}"));
    TEST_ASSERT_EQUAL(8, rq.tag.length);
    TEST_ASSERT_TRUE(parse("{\"unit\":1,\"area\":\"holding\",\"address\":50,\"type\":\"bytes\"}"));
    TEST_ASSERT_EQUAL(1, rq.count);
    TEST_ASSERT_EQUAL(1, rq.tag.length);
    TEST_ASSERT_TRUE(parse("{\"unit\":1,\"area\":\"coil\",\"address\":0,\"type\":\"bool\"}"));
    TEST_ASSERT_EQUAL(1, rq.count);
}

static void test_refused_requests(void)
{
    ASSERT_REFUSED("", "not valid JSON");
    ASSERT_REFUSED("[1]", "expected a JSON object");
    ASSERT_REFUSED("{\"area\":\"holding\",\"address\":0}", "\"unit\" is missing");
    ASSERT_REFUSED("{\"unit\":0,\"area\":\"holding\",\"address\":0}", "\"unit\" must be an integer 1-247");
    ASSERT_REFUSED("{\"unit\":248,\"area\":\"holding\",\"address\":0}", "1-247");
    ASSERT_REFUSED("{\"unit\":1.5,\"area\":\"holding\",\"address\":0}", "1-247");
    ASSERT_REFUSED("{\"unit\":\"1\",\"area\":\"holding\",\"address\":0}", "1-247");
    ASSERT_REFUSED("{\"unit\":1,\"address\":0}", "\"area\" must be coil, discrete, holding or input");
    ASSERT_REFUSED("{\"unit\":1,\"area\":\"holdings\",\"address\":0}", "\"area\" must be");
    ASSERT_REFUSED("{\"unit\":1,\"area\":3,\"address\":0}", "\"area\" must be a string");
    ASSERT_REFUSED("{\"unit\":1,\"area\":\"holding\"}", "\"address\" is missing");
    ASSERT_REFUSED("{\"unit\":1,\"area\":\"holding\",\"address\":65536}", "0-65535");
    ASSERT_REFUSED("{\"unit\":1,\"area\":\"holding\",\"address\":0,\"count\":17}", "\"count\" must be an integer 1-16");
    ASSERT_REFUSED("{\"unit\":1,\"area\":\"holding\",\"address\":0,\"count\":0}", "1-16");
    ASSERT_REFUSED("{\"unit\":1,\"area\":\"holding\",\"address\":65530,\"count\":7}", "past 65535");
    ASSERT_REFUSED("{\"unit\":1,\"area\":\"holding\",\"address\":0,\"cuont\":2}", "unknown key \"cuont\"");
    ASSERT_REFUSED("{\"unit\":1,\"area\":\"holding\",\"address\":0,\"type\":\"float16\"}", "\"type\" must be a tag type");
    ASSERT_REFUSED("{\"unit\":1,\"area\":\"holding\",\"address\":0,\"word_order\":\"CDAB\"}", "needs a \"type\"");
    ASSERT_REFUSED("{\"unit\":1,\"area\":\"holding\",\"address\":0,\"type\":\"int32\",\"word_order\":\"middle\"}",
                   "\"word_order\" must be");
    ASSERT_REFUSED("{\"unit\":1,\"area\":\"holding\",\"address\":0,\"type\":\"float64\",\"count\":3}",
                   "\"count\" 3 is less than the 4 registers of float64");
    ASSERT_REFUSED("{\"unit\":1,\"area\":\"coil\",\"address\":0,\"type\":\"uint16\"}", "the coil area holds bits");
    ASSERT_REFUSED("{\"unit\":1,\"area\":\"input\",\"address\":0,\"type\":\"bool\"}", "read the register as uint16");
}

static void test_answers(void)
{
    /* 230.5 as float32 ABCD (the E2E slave's input 0) */
    TEST_ASSERT_TRUE(parse("{\"unit\":1,\"area\":\"input\",\"address\":0,\"type\":\"float32\"}"));
    const uint16_t f[2] = {0x4366, 0x8000};
    TEST_ASSERT_EQUAL_STRING("200 OK", gw_mbdiag_answer(&rq, MB_OK, 0, f, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("{\"ok\":true,\"unit\":1,\"area\":\"input\",\"address\":0,\"count\":2,"
                             "\"values\":[17254,32768],\"type\":\"float32\",\"word_order\":\"ABCD\","
                             "\"value\":230.5}\n", out);
    /* the same registers as CDAB */
    TEST_ASSERT_TRUE(parse("{\"unit\":1,\"area\":\"input\",\"address\":0,\"type\":\"float32\","
                           "\"word_order\":\"little\"}"));
    const uint16_t fl[2] = {0x8000, 0x4366};
    gw_mbdiag_answer(&rq, MB_OK, 0, fl, out, sizeof(out));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "\"word_order\":\"CDAB\",\"value\":230.5}"), out);
    /* exact 64-bit integers */
    TEST_ASSERT_TRUE(parse("{\"unit\":1,\"area\":\"holding\",\"address\":0,\"type\":\"int64\"}"));
    const uint16_t i64[4] = {0xffff, 0xffff, 0xffff, 0xfffe};
    gw_mbdiag_answer(&rq, MB_OK, 0, i64, out, sizeof(out));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "\"value\":-2}"), out);
    TEST_ASSERT_TRUE(parse("{\"unit\":1,\"area\":\"holding\",\"address\":0,\"type\":\"uint64\"}"));
    gw_mbdiag_answer(&rq, MB_OK, 0, i64, out, sizeof(out));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "\"value\":18446744073709551614}"), out);
    /* a NaN is null (not JSON) */
    TEST_ASSERT_TRUE(parse("{\"unit\":1,\"area\":\"input\",\"address\":0,\"type\":\"float32\"}"));
    const uint16_t nan[2] = {0x7fc0, 0x0000};
    gw_mbdiag_answer(&rq, MB_OK, 0, nan, out, sizeof(out));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "\"value\":null}"), out);
    /* a string with a quote; bytes as hex */
    TEST_ASSERT_TRUE(parse("{\"unit\":1,\"area\":\"holding\",\"address\":50,\"type\":\"string\","
                           "\"count\":2}"));
    const uint16_t str[2] = {('S' << 8) | '"', ('\\' << 8) | 0};
    gw_mbdiag_answer(&rq, MB_OK, 0, str, out, sizeof(out));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "\"value\":\"S\\\"\\\\\"}"), out);
    TEST_ASSERT_TRUE(parse("{\"unit\":1,\"area\":\"holding\",\"address\":50,\"type\":\"bytes\","
                           "\"count\":2}"));
    const uint16_t raw[2] = {0x0102, 0xa0ff};
    gw_mbdiag_answer(&rq, MB_OK, 0, raw, out, sizeof(out));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "\"value\":\"0102a0ff\"}"), out);
    /* bits */
    TEST_ASSERT_TRUE(parse("{\"unit\":1,\"area\":\"coil\",\"address\":0,\"count\":3,\"type\":\"bool\"}"));
    const uint16_t bits[3] = {1, 0, 1};
    gw_mbdiag_answer(&rq, MB_OK, 0, bits, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("{\"ok\":true,\"unit\":1,\"area\":\"coil\",\"address\":0,\"count\":3,"
                             "\"values\":[1,0,1],\"type\":\"bool\",\"word_order\":\"ABCD\","
                             "\"value\":true}\n", out);
    /* the largest answer fits */
    TEST_ASSERT_TRUE(parse("{\"unit\":247,\"area\":\"holding\",\"address\":65520,\"count\":16,"
                           "\"type\":\"bytes\"}"));
    uint16_t big[16];
    for (int i = 0; i < 16; i++) {
        big[i] = 65535;
    }
    gw_mbdiag_answer(&rq, MB_OK, 0, big, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("}\n", out + strlen(out) - 2);
    TEST_ASSERT_LESS_THAN(512, strlen(out));
}

static void test_errors(void)
{
    TEST_ASSERT_TRUE(parse("{\"unit\":3,\"area\":\"holding\",\"address\":0}"));
    TEST_ASSERT_EQUAL_STRING("504 Gateway Timeout", gw_mbdiag_answer(&rq, MB_ERR_TIMEOUT, 0, NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("{\"ok\":false,\"result\":\"timeout\",\"error\":\"no answer from unit 3 "
                             "(timeout)\"}\n", out);
    TEST_ASSERT_EQUAL_STRING("502 Bad Gateway", gw_mbdiag_answer(&rq, MB_ERR_CRC, 0, NULL, out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"result\":\"crc\""));
    TEST_ASSERT_EQUAL_STRING("502 Bad Gateway", gw_mbdiag_answer(&rq, MB_ERR_FRAME, 0, NULL, out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"result\":\"frame\""));
    TEST_ASSERT_EQUAL_STRING("502 Bad Gateway", gw_mbdiag_answer(&rq, MB_ERR_EXCEPTION, 2, NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("{\"ok\":false,\"result\":\"exception\",\"exception\":2,\"error\":\"unit 3: "
                             "Modbus exception 2 (illegal data address)\"}\n", out);
    TEST_ASSERT_EQUAL_STRING("503 Service Unavailable", gw_mbdiag_answer(&rq, MB_ERR_ARG, 0, NULL, out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"result\":\"busy\""));
    /* a small buffer cuts the answer, never overruns it */
    char tiny[8];
    memset(tiny, 'x', sizeof(tiny));
    gw_mbdiag_answer(&rq, MB_ERR_TIMEOUT, 0, NULL, tiny, sizeof(tiny));
    TEST_ASSERT_EQUAL(7, strlen(tiny));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_plain_reads);
    RUN_TEST(test_typed_reads);
    RUN_TEST(test_refused_requests);
    RUN_TEST(test_answers);
    RUN_TEST(test_errors);
    return UNITY_END();
}
