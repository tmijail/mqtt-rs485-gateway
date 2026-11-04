/* QA-1: Sparkplug B payload encoding/decoding (SPB-1). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "spb.h"
#include "support.h"
#include "unity.h"

static uint8_t buf[GW_SPB_PAYLOAD_MAX];
static decoded_t dec;

void setUp(void) {}
void tearDown(void) {}

static void test_round_trip_all_types(void)
{
    spb_metric_t m[] = {
        {.name = "b", .alias = 1, .datatype = SPB_DT_BOOLEAN, .timestamp = 5, .v.num = 1},
        {.name = "i16", .alias = 2, .datatype = SPB_DT_INT16, .v.num = -5},
        {.name = "u16", .alias = 3, .datatype = SPB_DT_UINT16, .v.num = 65535},
        {.name = "i32", .alias = 4, .datatype = SPB_DT_INT32, .v.num = -2147483648.0},
        {.name = "u32", .alias = 5, .datatype = SPB_DT_UINT32, .v.num = 4294967295.0},
        {.name = "f", .alias = 6, .datatype = SPB_DT_FLOAT, .v.num = 230.5},
        {.name = "u64", .alias = 7, .datatype = SPB_DT_UINT64, .v.u64 = 255},
        {.name = "s", .alias = 8, .datatype = SPB_DT_STRING, .v.str = "1.2.3+gabc"},
        {.name = NULL, .alias = 9, .datatype = SPB_DT_FLOAT, .is_null = true},
    };
    spb_payload_t p = {.timestamp = 1700000000123ull, .has_seq = true, .seq = 42,
                       .metrics = m, .metric_count = 9};
    size_t len = spb_encode(&p, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_TRUE(decode_payload(buf, len, &dec));
    TEST_ASSERT_TRUE(dec.has_seq);
    TEST_ASSERT_EQUAL_UINT64(42, dec.seq);
    TEST_ASSERT_EQUAL_UINT64(1700000000123ull, dec.timestamp);
    TEST_ASSERT_EQUAL(9, dec.count);
    TEST_ASSERT_EQUAL_DOUBLE(1, dec.m[0].num);
    TEST_ASSERT_EQUAL_UINT64(5, dec.m[0].timestamp);
    TEST_ASSERT_EQUAL(SPB_VAL_BOOL, dec.m[0].kind);
    TEST_ASSERT_EQUAL_DOUBLE(-5, dec.m[1].num);
    TEST_ASSERT_EQUAL(SPB_VAL_INT, dec.m[1].kind);
    TEST_ASSERT_EQUAL_DOUBLE(65535, dec.m[2].num);
    TEST_ASSERT_EQUAL_DOUBLE(-2147483648.0, dec.m[3].num);
    TEST_ASSERT_EQUAL_DOUBLE(4294967295.0, dec.m[4].num);
    TEST_ASSERT_EQUAL_DOUBLE(230.5, dec.m[5].num);
    TEST_ASSERT_EQUAL(SPB_VAL_FLOAT, dec.m[5].kind);
    TEST_ASSERT_EQUAL(SPB_VAL_LONG, dec.m[6].kind);
    TEST_ASSERT_EQUAL_STRING("1.2.3+gabc", dec.m[7].str);
    TEST_ASSERT_FALSE(dec.m[8].has_name);
    TEST_ASSERT_TRUE(dec.m[8].is_null);
    TEST_ASSERT_EQUAL(SPB_VAL_NONE, dec.m[8].kind);
    TEST_ASSERT_EQUAL_UINT64(9, dec.m[8].alias);
}

/* MB-6: 64-bit integers travel exactly in long_value (two's complement for Int64), Double in
 * double_value. */
static void test_round_trip_64bit_exact(void)
{
    spb_metric_t m[] = {
        {.alias = 1, .datatype = SPB_DT_INT64, .v.i64 = -9007199254740993ll},
        {.alias = 2, .datatype = SPB_DT_UINT64, .v.u64 = UINT64_MAX},
        {.alias = 3, .datatype = SPB_DT_DOUBLE, .v.num = 1234.56789012345},
        {.alias = 4, .datatype = SPB_DT_INT64, .v.i64 = INT64_MIN},
    };
    spb_payload_t p = {.timestamp = 1, .has_seq = true, .seq = 0, .metrics = m, .metric_count = 4};
    size_t len = spb_encode(&p, buf, sizeof(buf));
    TEST_ASSERT_TRUE(decode_payload(buf, len, &dec));
    TEST_ASSERT_EQUAL(SPB_VAL_LONG, dec.m[0].kind);
    TEST_ASSERT_EQUAL_INT64(-9007199254740993ll, (int64_t)dec.m[0].u64);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, dec.m[1].u64);
    TEST_ASSERT_EQUAL(SPB_VAL_DOUBLE, dec.m[2].kind);
    TEST_ASSERT_EQUAL_DOUBLE(1234.56789012345, dec.m[2].num);
    TEST_ASSERT_EQUAL_UINT64(0x8000000000000000ull, dec.m[3].u64);
}

static void test_wire_format_int16_sign_extension(void)
{
    /* Sparkplug B: Int16 -5 is carried in int_value as the 32-bit two's complement 0xFFFFFFFB. */
    spb_metric_t m = {.alias = 1, .datatype = SPB_DT_INT16, .v.num = -5};
    spb_payload_t p = {.timestamp = 1, .has_seq = true, .seq = 0, .metrics = &m, .metric_count = 1};
    size_t len = spb_encode(&p, buf, sizeof(buf));
    /* metric submessage: alias(2)=1, datatype(4)=2, int_value(10)=0xFFFFFFFB as varint */
    const uint8_t expect[] = {0x10, 0x01, 0x20, 0x02, 0x50, 0xFB, 0xFF, 0xFF, 0xFF, 0x0F};
    TEST_ASSERT_GREATER_OR_EQUAL(sizeof(expect), len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, buf + len - sizeof(expect) - 2, sizeof(expect));
}

static void test_ndeath_has_no_seq(void)
{
    size_t len = gw_node_death_payload(7, buf, sizeof(buf));
    TEST_ASSERT_TRUE(decode_payload(buf, len, &dec));
    TEST_ASSERT_FALSE(dec.has_seq);
    TEST_ASSERT_EQUAL(1, dec.count);
    TEST_ASSERT_EQUAL_STRING("bdSeq", dec.m[0].name);
    TEST_ASSERT_EQUAL(SPB_DT_UINT64, dec.m[0].datatype);
    TEST_ASSERT_EQUAL_DOUBLE(7, dec.m[0].num);
}

static void test_rx_number_variants(void)
{
    spb_rx_metric_t m = {.kind = SPB_VAL_INT, .v.i = 0xFFFB};  /* not sign-extended by sender */
    double v;
    TEST_ASSERT_TRUE(spb_rx_number(&m, SPB_DT_INT16, &v));
    TEST_ASSERT_EQUAL_DOUBLE(-5, v);
    m.v.i = 0xFFFFFFFB;
    TEST_ASSERT_TRUE(spb_rx_number(&m, SPB_DT_INT16, &v));
    TEST_ASSERT_EQUAL_DOUBLE(-5, v);
    TEST_ASSERT_TRUE(spb_rx_number(&m, SPB_DT_UINT32, &v));
    TEST_ASSERT_EQUAL_DOUBLE(4294967291.0, v);
    m.kind = SPB_VAL_FLOAT;
    m.v.f = 12.3f;
    TEST_ASSERT_TRUE(spb_rx_number(&m, SPB_DT_FLOAT, &v));
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 12.3, v);
    m.kind = SPB_VAL_BOOL;
    m.v.b = true;
    TEST_ASSERT_TRUE(spb_rx_number(&m, SPB_DT_BOOLEAN, &v));
    TEST_ASSERT_EQUAL_DOUBLE(1, v);
    m.kind = SPB_VAL_STRING;
    TEST_ASSERT_FALSE(spb_rx_number(&m, SPB_DT_STRING, &v));
    m.kind = SPB_VAL_DOUBLE;
    m.is_null = true;
    TEST_ASSERT_FALSE(spb_rx_number(&m, SPB_DT_DOUBLE, &v));
}

static void test_decode_rejects_garbage(void)
{
    const uint8_t junk[] = {0x12, 0xFF, 0x01, 0x02};  /* metrics field with impossible length */
    TEST_ASSERT_FALSE(decode_payload(junk, sizeof(junk), &dec));
    /* unknown/ignored fields (uuid, bytes_value) are skipped */
    const uint8_t with_uuid[] = {0x08, 0x01, 0x22, 0x03, 'a', 'b', 'c', 0x18, 0x05};
    TEST_ASSERT_TRUE(decode_payload(with_uuid, sizeof(with_uuid), &dec));
    TEST_ASSERT_EQUAL_UINT64(5, dec.seq);
}

static void test_worst_case_birth_fits(void)
{
    static spb_metric_t m[GW_MAX_TAGS_PER_DEVICE];
    char name[40];
    memset(name, 'x', 39);
    name[39] = '\0';
    for (int i = 0; i < GW_MAX_TAGS_PER_DEVICE; i++) {
        m[i] = (spb_metric_t){.name = name, .alias = 100 + 255, .datatype = SPB_DT_INT32,
                              .timestamp = 1900000000000ull, .v.num = -1};
    }
    spb_payload_t p = {.timestamp = 1900000000000ull, .has_seq = true, .seq = 255, .metrics = m,
                       .metric_count = GW_MAX_TAGS_PER_DEVICE};
    size_t len = spb_encode(&p, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_LESS_THAN(GW_SPB_PAYLOAD_MAX, len);
    TEST_ASSERT_EQUAL(0, spb_encode(&p, buf, len - 1));  /* overflow is detected, not truncated */
}

static void test_topics(void)
{
    char t[GW_TOPIC_MAX];
    TEST_ASSERT_TRUE(spb_topic(t, sizeof(t), "Plant1", "NBIRTH", "GW01", NULL));
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/NBIRTH/GW01", t);
    TEST_ASSERT_TRUE(spb_topic(t, sizeof(t), "Plant1", "DDATA", "GW01", "Meter1"));
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/DDATA/GW01/Meter1", t);
    TEST_ASSERT_FALSE(spb_topic(t, 10, "Plant1", "DDATA", "GW01", "Meter1"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_round_trip_all_types);
    RUN_TEST(test_round_trip_64bit_exact);
    RUN_TEST(test_wire_format_int16_sign_extension);
    RUN_TEST(test_ndeath_has_no_seq);
    RUN_TEST(test_rx_number_variants);
    RUN_TEST(test_decode_rejects_garbage);
    RUN_TEST(test_worst_case_birth_fits);
    RUN_TEST(test_topics);
    return UNITY_END();
}
