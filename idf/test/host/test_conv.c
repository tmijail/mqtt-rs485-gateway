/* QA-1: type and scale conversion (MB-5, MB-7) and the Sparkplug datatype mapping. */
#include <math.h>
#include <string.h>

#include "gw_conv.h"
#include "spb.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static bool keep_metric(void *ctx, const spb_rx_metric_t *m)
{
    *(spb_rx_metric_t *)ctx = *m;
    return true;
}

static gw_tag_cfg_t tag(gw_type_t type, gw_word_order_t wo, double scale, double offset)
{
    gw_tag_cfg_t t;
    memset(&t, 0, sizeof(t));
    t.area = type == GW_TYPE_BOOL ? GW_AREA_COIL : GW_AREA_HOLDING;
    t.type = type;
    t.word_order = wo;
    t.scale = scale;
    t.offset = offset;
    return t;
}

static void test_datatype_mapping(void)
{
    gw_tag_cfg_t t = tag(GW_TYPE_BOOL, GW_WORD_BIG, 1, 0);
    TEST_ASSERT_EQUAL(SPB_DT_BOOLEAN, gw_conv_datatype(&t));
    t = tag(GW_TYPE_INT16, GW_WORD_BIG, 1, 0);
    TEST_ASSERT_EQUAL(SPB_DT_INT16, gw_conv_datatype(&t));
    t = tag(GW_TYPE_UINT16, GW_WORD_BIG, 1, 0);
    TEST_ASSERT_EQUAL(SPB_DT_UINT16, gw_conv_datatype(&t));
    t = tag(GW_TYPE_INT32, GW_WORD_BIG, 1, 0);
    TEST_ASSERT_EQUAL(SPB_DT_INT32, gw_conv_datatype(&t));
    t = tag(GW_TYPE_UINT32, GW_WORD_BIG, 1, 0);
    TEST_ASSERT_EQUAL(SPB_DT_UINT32, gw_conv_datatype(&t));
    t = tag(GW_TYPE_FLOAT32, GW_WORD_BIG, 1, 0);
    TEST_ASSERT_EQUAL(SPB_DT_FLOAT, gw_conv_datatype(&t));
    t = tag(GW_TYPE_INT16, GW_WORD_BIG, 0.1, 0);   /* scaled -> Float */
    TEST_ASSERT_EQUAL(SPB_DT_FLOAT, gw_conv_datatype(&t));
    t = tag(GW_TYPE_UINT16, GW_WORD_BIG, 1, -5);   /* offset -> Float */
    TEST_ASSERT_EQUAL(SPB_DT_FLOAT, gw_conv_datatype(&t));
    t = tag(GW_TYPE_FLOAT32, GW_WORD_BIG, 2, 0);   /* a scaled float32 stays Float */
    TEST_ASSERT_EQUAL(SPB_DT_FLOAT, gw_conv_datatype(&t));
    /* MB-6 (#165): scaled 32-bit integer sources -> Double (a Float has a 24-bit mantissa) */
    t = tag(GW_TYPE_UINT32, GW_WORD_BIG, 1, -5);
    TEST_ASSERT_EQUAL(SPB_DT_DOUBLE, gw_conv_datatype(&t));
    TEST_ASSERT_EQUAL(GW_VK_DOUBLE, gw_conv_kind(&t));
    t = tag(GW_TYPE_INT32, GW_WORD_BIG, 0.01, 0);
    TEST_ASSERT_EQUAL(SPB_DT_DOUBLE, gw_conv_datatype(&t));
}

/* MB-6 (#165): a scaled uint32 above 2^24 keeps every digit through the conversion and the
 * Double of the codec (a Float would give 4 000 000 000). */
static void test_scaled_32bit_keeps_precision(void)
{
    gw_tag_cfg_t t = tag(GW_TYPE_UINT32, GW_WORD_BIG, 1.0, 1.0);
    const uint16_t r[2] = {(uint16_t)(4000000000u >> 16), (uint16_t)(4000000000u & 0xFFFFu)};
    gw_tag_value_t v;
    memset(&v, 0, sizeof(v));
    gw_conv_decode(&t, r, &v);
    TEST_ASSERT_EQUAL_DOUBLE(4000000001.0, v.value);
    spb_metric_t m = {.name = "Energy", .alias = 100, .datatype = gw_conv_datatype(&t),
                      .timestamp = 1, .v.num = v.value};
    spb_payload_t p = {.timestamp = 1, .has_seq = true, .seq = 0, .metrics = &m,
                       .metric_count = 1};
    uint8_t buf[128];
    size_t len = spb_encode(&p, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, len);
    static spb_rx_metric_t got;
    TEST_ASSERT_TRUE(spb_decode(buf, len, NULL, keep_metric, &got));
    TEST_ASSERT_EQUAL(SPB_VAL_DOUBLE, got.kind);
    TEST_ASSERT_EQUAL_DOUBLE(4000000001.0, got.v.d);
    /* the inverse conversion of a DCMD value is exact too */
    uint16_t w[2];
    TEST_ASSERT_EQUAL(GW_CONV_OK, gw_conv_to_regs(&t, 4000000001.0, w));
    TEST_ASSERT_EQUAL_HEX16(r[0], w[0]);
    TEST_ASSERT_EQUAL_HEX16(r[1], w[1]);
}

static void test_decode_16bit(void)
{
    gw_tag_cfg_t t = tag(GW_TYPE_INT16, GW_WORD_BIG, 1, 0);
    uint16_t r[2] = {0xFFFE, 0};
    TEST_ASSERT_EQUAL_DOUBLE(-2.0, gw_conv_from_regs(&t, r));
    t.type = GW_TYPE_UINT16;
    TEST_ASSERT_EQUAL_DOUBLE(65534.0, gw_conv_from_regs(&t, r));
    t = tag(GW_TYPE_INT16, GW_WORD_BIG, 0.1, 0);
    r[0] = 123;
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 12.3, gw_conv_from_regs(&t, r));
    t = tag(GW_TYPE_UINT16, GW_WORD_BIG, 0.5, -10);
    r[0] = 100;
    TEST_ASSERT_EQUAL_DOUBLE(40.0, gw_conv_from_regs(&t, r));
}

static void test_decode_32bit_word_order(void)
{
    gw_tag_cfg_t t = tag(GW_TYPE_UINT32, GW_WORD_BIG, 1, 0);
    uint16_t abcd[2] = {0x0001, 0x0002};
    TEST_ASSERT_EQUAL_DOUBLE(65538.0, gw_conv_from_regs(&t, abcd));
    t.word_order = GW_WORD_LITTLE;
    TEST_ASSERT_EQUAL_DOUBLE(131073.0, gw_conv_from_regs(&t, abcd));

    t = tag(GW_TYPE_INT32, GW_WORD_BIG, 1, 0);
    uint16_t neg[2] = {0xFFFF, 0xFFF6};
    TEST_ASSERT_EQUAL_DOUBLE(-10.0, gw_conv_from_regs(&t, neg));

    /* 230.5f = 0x43668000 */
    t = tag(GW_TYPE_FLOAT32, GW_WORD_BIG, 1, 0);
    uint16_t f_big[2] = {0x4366, 0x8000};
    TEST_ASSERT_EQUAL_DOUBLE(230.5, gw_conv_from_regs(&t, f_big));
    t.word_order = GW_WORD_LITTLE;
    uint16_t f_little[2] = {0x8000, 0x4366};
    TEST_ASSERT_EQUAL_DOUBLE(230.5, gw_conv_from_regs(&t, f_little));
}

static void test_encode_inverse_scale(void)
{
    /* SPEC acceptance 4: Setpoint int16 scale 0.1, value 12.3 -> raw 123 */
    gw_tag_cfg_t t = tag(GW_TYPE_INT16, GW_WORD_BIG, 0.1, 0);
    uint16_t r[2] = {0, 0};
    TEST_ASSERT_EQUAL(GW_CONV_OK, gw_conv_to_regs(&t, 12.3, r));
    TEST_ASSERT_EQUAL(123, r[0]);
    TEST_ASSERT_EQUAL(GW_CONV_OK, gw_conv_to_regs(&t, (double)12.3f, r)); /* from a Float metric */
    TEST_ASSERT_EQUAL(123, r[0]);
    TEST_ASSERT_EQUAL(GW_CONV_OK, gw_conv_to_regs(&t, -0.26, r));
    TEST_ASSERT_EQUAL_HEX16((uint16_t)-3, r[0]);
    TEST_ASSERT_EQUAL(GW_CONV_RANGE, gw_conv_to_regs(&t, 3276.8, r));
    TEST_ASSERT_EQUAL(GW_CONV_OK, gw_conv_to_regs(&t, 3276.7, r));
    TEST_ASSERT_EQUAL(GW_CONV_NOT_FINITE, gw_conv_to_regs(&t, NAN, r));

    t = tag(GW_TYPE_UINT16, GW_WORD_BIG, 1, 0);
    TEST_ASSERT_EQUAL(GW_CONV_RANGE, gw_conv_to_regs(&t, -1, r));
    TEST_ASSERT_EQUAL(GW_CONV_RANGE, gw_conv_to_regs(&t, 65536, r));

    t = tag(GW_TYPE_UINT16, GW_WORD_BIG, 0.5, -10);
    TEST_ASSERT_EQUAL(GW_CONV_OK, gw_conv_to_regs(&t, 40.0, r));
    TEST_ASSERT_EQUAL(100, r[0]);
}

static void test_encode_32bit_round_trip(void)
{
    const gw_word_order_t orders[2] = {GW_WORD_BIG, GW_WORD_LITTLE};
    for (int o = 0; o < 2; o++) {
        gw_tag_cfg_t t = tag(GW_TYPE_INT32, orders[o], 1, 0);
        uint16_t r[2];
        TEST_ASSERT_EQUAL(GW_CONV_OK, gw_conv_to_regs(&t, -123456789, r));
        TEST_ASSERT_EQUAL_DOUBLE(-123456789.0, gw_conv_from_regs(&t, r));
        t.type = GW_TYPE_UINT32;
        TEST_ASSERT_EQUAL(GW_CONV_OK, gw_conv_to_regs(&t, 4294967295.0, r));
        TEST_ASSERT_EQUAL_DOUBLE(4294967295.0, gw_conv_from_regs(&t, r));
        TEST_ASSERT_EQUAL(GW_CONV_RANGE, gw_conv_to_regs(&t, 4294967296.0, r));
        t.type = GW_TYPE_FLOAT32;
        TEST_ASSERT_EQUAL(GW_CONV_OK, gw_conv_to_regs(&t, 230.5, r));
        TEST_ASSERT_EQUAL_HEX16(orders[o] == GW_WORD_BIG ? 0x4366 : 0x8000, r[0]);
        TEST_ASSERT_EQUAL_DOUBLE(230.5, gw_conv_from_regs(&t, r));
        TEST_ASSERT_EQUAL(GW_CONV_RANGE, gw_conv_to_regs(&t, 1e39, r));
    }
}

/* ---- MB-6 ---- */

static void test_datatype_mapping_mb6(void)
{
    gw_tag_cfg_t t = tag(GW_TYPE_INT64, GW_WORD_BIG, 1, 0);
    TEST_ASSERT_EQUAL(SPB_DT_INT64, gw_conv_datatype(&t));
    TEST_ASSERT_EQUAL(GW_VK_INT64, gw_conv_kind(&t));
    t = tag(GW_TYPE_UINT64, GW_WORD_BIG, 1, 0);
    TEST_ASSERT_EQUAL(SPB_DT_UINT64, gw_conv_datatype(&t));
    TEST_ASSERT_EQUAL(GW_VK_UINT64, gw_conv_kind(&t));
    t = tag(GW_TYPE_FLOAT64, GW_WORD_BIG, 1, 0);
    TEST_ASSERT_EQUAL(SPB_DT_DOUBLE, gw_conv_datatype(&t));
    TEST_ASSERT_EQUAL(GW_VK_DOUBLE, gw_conv_kind(&t));
    t = tag(GW_TYPE_STRING, GW_WORD_BIG, 1, 0);
    TEST_ASSERT_EQUAL(SPB_DT_STRING, gw_conv_datatype(&t));
    TEST_ASSERT_EQUAL(GW_VK_STRING, gw_conv_kind(&t));
    /* scaled 64-bit sources: Double, not Float, to keep their precision */
    t = tag(GW_TYPE_INT64, GW_WORD_BIG, 0.001, 0);
    TEST_ASSERT_EQUAL(SPB_DT_DOUBLE, gw_conv_datatype(&t));
    TEST_ASSERT_EQUAL(GW_VK_DOUBLE, gw_conv_kind(&t));
    t = tag(GW_TYPE_UINT64, GW_WORD_BIG, 1, 1);
    TEST_ASSERT_EQUAL(SPB_DT_DOUBLE, gw_conv_datatype(&t));
    t = tag(GW_TYPE_INT16, GW_WORD_BIG, 2, 0);   /* the MVP rule stays for 16 bits: Float */
    TEST_ASSERT_EQUAL(SPB_DT_FLOAT, gw_conv_datatype(&t));
}

static void test_byte_orders_32bit(void)
{
    /* 0x11223344 in each order: ABCD 1122 3344, CDAB 3344 1122, BADC 2211 4433, DCBA 4433 2211 */
    const struct {
        gw_word_order_t o;
        uint16_t r[2];
    } cases[] = {{GW_WORD_BIG, {0x1122, 0x3344}},
                 {GW_WORD_LITTLE, {0x3344, 0x1122}},
                 {GW_WORD_BADC, {0x2211, 0x4433}},
                 {GW_WORD_DCBA, {0x4433, 0x2211}}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        gw_tag_cfg_t t = tag(GW_TYPE_UINT32, cases[i].o, 1, 0);
        TEST_ASSERT_EQUAL_DOUBLE((double)0x11223344u, gw_conv_from_regs(&t, cases[i].r));
        uint16_t r[2];
        TEST_ASSERT_EQUAL(GW_CONV_OK, gw_conv_to_regs(&t, (double)0x11223344u, r));
        TEST_ASSERT_EQUAL_HEX16_ARRAY(cases[i].r, r, 2);
    }
    /* 16-bit: only the byte swap matters */
    gw_tag_cfg_t t = tag(GW_TYPE_INT16, GW_WORD_DCBA, 1, 0);
    uint16_t r[1] = {0xFEFF};   /* 0xFFFE byte swapped */
    TEST_ASSERT_EQUAL_DOUBLE(-2.0, gw_conv_from_regs(&t, r));
    t.word_order = GW_WORD_LITTLE;
    r[0] = 0xFFFE;
    TEST_ASSERT_EQUAL_DOUBLE(-2.0, gw_conv_from_regs(&t, r));
}

static void test_64bit_exact_and_orders(void)
{
    /* 2^53 + 1 is not representable in a double: it must survive exactly */
    const uint64_t big = 9007199254740993ull;   /* 0x0020000000000001 */
    const struct {
        gw_word_order_t o;
        uint16_t r[4];
    } cases[] = {{GW_WORD_BIG, {0x0020, 0x0000, 0x0000, 0x0001}},
                 {GW_WORD_LITTLE, {0x0001, 0x0000, 0x0000, 0x0020}},
                 {GW_WORD_BADC, {0x2000, 0x0000, 0x0000, 0x0100}},
                 {GW_WORD_DCBA, {0x0100, 0x0000, 0x0000, 0x2000}}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        gw_tag_cfg_t t = tag(GW_TYPE_UINT64, cases[i].o, 1, 0);
        gw_tag_value_t v;
        gw_conv_decode(&t, cases[i].r, &v);
        TEST_ASSERT_EQUAL_UINT64(big, v.u64);
        uint16_t r[4];
        TEST_ASSERT_EQUAL(GW_CONV_OK, gw_conv_encode(&t, &v, r));
        TEST_ASSERT_EQUAL_HEX16_ARRAY(cases[i].r, r, 4);
    }
    gw_tag_cfg_t t = tag(GW_TYPE_INT64, GW_WORD_LITTLE, 1, 0);
    gw_tag_value_t v = {.i64 = -9007199254740993ll};
    uint16_t r[4];
    TEST_ASSERT_EQUAL(GW_CONV_OK, gw_conv_encode(&t, &v, r));
    gw_tag_value_t back;
    gw_conv_decode(&t, r, &back);
    TEST_ASSERT_EQUAL_INT64(-9007199254740993ll, back.i64);
    v.i64 = INT64_MIN;
    gw_conv_encode(&t, &v, r);
    gw_conv_decode(&t, r, &back);
    TEST_ASSERT_EQUAL_INT64(INT64_MIN, back.i64);
    /* from a double: only integral, in-range values */
    TEST_ASSERT_TRUE(gw_conv_from_double(&t, -5.0, &v));
    TEST_ASSERT_EQUAL_INT64(-5, v.i64);
    TEST_ASSERT_FALSE(gw_conv_from_double(&t, 1.5, &v));
    TEST_ASSERT_FALSE(gw_conv_from_double(&t, 9223372036854775808.0, &v));
    t.type = GW_TYPE_UINT64;
    TEST_ASSERT_FALSE(gw_conv_from_double(&t, -1.0, &v));
    TEST_ASSERT_EQUAL(GW_CONV_RANGE, gw_conv_to_regs(&t, 18446744073709551616.0, r));
    /* scaled int64: a double, range-checked after the inverse scale */
    t = tag(GW_TYPE_INT64, GW_WORD_BIG, 0.5, 0);
    TEST_ASSERT_EQUAL(GW_CONV_OK, gw_conv_to_regs(&t, -3.5, r));
    TEST_ASSERT_EQUAL_DOUBLE(-3.5, gw_conv_from_regs(&t, r));
    TEST_ASSERT_EQUAL(GW_CONV_RANGE, gw_conv_to_regs(&t, 1e19, r));
}

static void test_float64(void)
{
    /* 1234.56789012345 as IEEE 754 binary64, in DCBA (fully little-endian) order */
    double d = 1234.56789012345;
    uint64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    gw_tag_cfg_t t = tag(GW_TYPE_FLOAT64, GW_WORD_DCBA, 1, 0);
    uint16_t r[4];
    TEST_ASSERT_EQUAL(GW_CONV_OK, gw_conv_to_regs(&t, d, r));
    for (int i = 0; i < 4; i++) {
        uint16_t w = (uint16_t)(bits >> (16 * i));          /* least significant word first */
        TEST_ASSERT_EQUAL_HEX16((uint16_t)((w << 8) | (w >> 8)), r[i]);   /* bytes swapped */
    }
    TEST_ASSERT_EQUAL_DOUBLE(d, gw_conv_from_regs(&t, r));
    TEST_ASSERT_EQUAL(GW_CONV_NOT_FINITE, gw_conv_to_regs(&t, INFINITY, r));
    t.scale = 1e300;
    TEST_ASSERT_EQUAL(GW_CONV_OK, gw_conv_to_regs(&t, 1e300, r));
    t.scale = 1e-300;
    TEST_ASSERT_EQUAL(GW_CONV_RANGE, gw_conv_to_regs(&t, 1e300, r));   /* raw overflows */
}

static void test_strings(void)
{
    gw_tag_cfg_t t = tag(GW_TYPE_STRING, GW_WORD_BIG, 1, 0);
    t.length = 4;
    uint16_t r[4] = {0x534E, 0x2D34, 0x3220, 0x2020};   /* "SN-42   " */
    gw_tag_value_t v;
    gw_conv_decode(&t, r, &v);
    TEST_ASSERT_EQUAL_STRING("SN-42", v.str);           /* padding dropped */
    r[1] = 0x2D00;                                      /* NUL ends the text */
    gw_conv_decode(&t, r, &v);
    TEST_ASSERT_EQUAL_STRING("SN-", v.str);
    r[1] = 0x01FF;                                      /* not printable ASCII */
    gw_conv_decode(&t, r, &v);
    TEST_ASSERT_EQUAL_STRING("SN??2", v.str);
    t.word_order = GW_WORD_BADC;                        /* first character in the low byte */
    uint16_t sw[4] = {0x4E53, 0x342D, 0x2032, 0x0000};
    gw_conv_decode(&t, sw, &v);
    TEST_ASSERT_EQUAL_STRING("SN-42", v.str);
    strcpy(v.str, "HELLO");
    uint16_t out[4];
    TEST_ASSERT_EQUAL(GW_CONV_OK, gw_conv_encode(&t, &v, out));
    const uint16_t hello_badc[4] = {0x4548, 0x4C4C, 0x004F, 0x0000};
    TEST_ASSERT_EQUAL_HEX16_ARRAY(hello_badc, out, 4);
    t.word_order = GW_WORD_BIG;
    strcpy(v.str, "12345678");                          /* exactly 2 x length */
    TEST_ASSERT_EQUAL(GW_CONV_OK, gw_conv_encode(&t, &v, out));
    TEST_ASSERT_EQUAL_HEX16(0x3738, out[3]);
    strcpy(v.str, "123456789");
    TEST_ASSERT_EQUAL(GW_CONV_RANGE, gw_conv_encode(&t, &v, out));
    TEST_ASSERT_FALSE(gw_conv_from_double(&t, 1.0, &v));
    /* the longest string: 16 registers, 32 characters */
    t.length = GW_STR_MAX_REGS;
    uint16_t full[GW_STR_MAX_REGS];
    for (int i = 0; i < GW_STR_MAX_REGS; i++) {
        full[i] = 0x4142;
    }
    gw_conv_decode(&t, full, &v);
    TEST_ASSERT_EQUAL(GW_STR_MAX, strlen(v.str));
}

/* SPB-19: a bytes tag carries its registers as raw bytes (NUL and non-ASCII included). */
static void test_bytes(void)
{
    gw_tag_cfg_t t = tag(GW_TYPE_BYTES, GW_WORD_BIG, 1, 0);
    t.length = 3;
    TEST_ASSERT_EQUAL(SPB_DT_BYTES, gw_conv_datatype(&t));
    TEST_ASSERT_EQUAL(GW_VK_BYTES, gw_conv_kind(&t));
    TEST_ASSERT_EQUAL(3, gw_tag_width(&t));
    uint16_t r[3] = {0x00FF, 0x1234, 0x8000};
    gw_tag_value_t v;
    gw_conv_decode(&t, r, &v);
    const uint8_t big[6] = {0x00, 0xFF, 0x12, 0x34, 0x80, 0x00};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(big, v.str, 6);
    char text[16];
    gw_conv_format(&t, &v, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("00ff12348000", text);
    t.word_order = GW_WORD_DCBA;                  /* bytes swapped in each register only */
    gw_conv_decode(&t, r, &v);
    const uint8_t swapped[6] = {0xFF, 0x00, 0x34, 0x12, 0x00, 0x80};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(swapped, v.str, 6);
    uint16_t out[3];
    TEST_ASSERT_EQUAL(GW_CONV_OK, gw_conv_encode(&t, &v, out));
    TEST_ASSERT_EQUAL_HEX16_ARRAY(r, out, 3);    /* round trip */
    TEST_ASSERT_FALSE(gw_conv_from_double(&t, 1.0, &v));
}

/* #196: the write limits, exact for 64-bit integers and at Float precision for Float tags */
static void test_write_limits(void)
{
    gw_tag_value_t v;
    memset(&v, 0, sizeof(v));
    gw_tag_cfg_t t = tag(GW_TYPE_INT16, GW_WORD_BIG, 0.1, 0);   /* a Float metric */
    v.value = 1e9;
    TEST_ASSERT_TRUE(gw_conv_in_write_limits(&t, &v));          /* no limits */
    t.has_write_min = t.has_write_max = true;
    t.write_min = 0.1;
    t.write_max = 0.3;
    v.value = (double)0.1f;                                      /* the Float a host sends */
    TEST_ASSERT_TRUE(gw_conv_in_write_limits(&t, &v));
    v.value = (double)0.3f;
    TEST_ASSERT_TRUE(gw_conv_in_write_limits(&t, &v));
    v.value = 0.31;
    TEST_ASSERT_FALSE(gw_conv_in_write_limits(&t, &v));
    v.value = 0.09;
    TEST_ASSERT_FALSE(gw_conv_in_write_limits(&t, &v));
    v.value = NAN;
    TEST_ASSERT_FALSE(gw_conv_in_write_limits(&t, &v));
    v.value = INFINITY;
    TEST_ASSERT_FALSE(gw_conv_in_write_limits(&t, &v));
    t.has_write_min = false;                                     /* only an upper limit */
    v.value = -1e30;
    TEST_ASSERT_TRUE(gw_conv_in_write_limits(&t, &v));

    t = tag(GW_TYPE_FLOAT64, GW_WORD_BIG, 1, 0);                 /* a Double metric: exact */
    t.has_write_max = true;
    t.write_max = 0.1;
    v.value = 0.1;
    TEST_ASSERT_TRUE(gw_conv_in_write_limits(&t, &v));
    v.value = (double)0.1f;                                      /* 0.10000000149 */
    TEST_ASSERT_FALSE(gw_conv_in_write_limits(&t, &v));

    t = tag(GW_TYPE_INT64, GW_WORD_BIG, 1, 0);
    t.has_write_min = t.has_write_max = true;
    t.write_min = -9007199254740992.0;                           /* -2^53 */
    t.write_max = 9007199254740992.0;                            /* 2^53 */
    v.i64 = 9007199254740992ll;
    TEST_ASSERT_TRUE(gw_conv_in_write_limits(&t, &v));
    v.i64 = 9007199254740993ll;                                  /* the same double as 2^53 */
    TEST_ASSERT_FALSE(gw_conv_in_write_limits(&t, &v));
    v.i64 = -9007199254740993ll;
    TEST_ASSERT_FALSE(gw_conv_in_write_limits(&t, &v));
    t.write_min = -2.5;                                          /* a limit with a fraction */
    t.write_max = 2.5;
    v.i64 = -2;
    TEST_ASSERT_TRUE(gw_conv_in_write_limits(&t, &v));
    v.i64 = 2;
    TEST_ASSERT_TRUE(gw_conv_in_write_limits(&t, &v));
    v.i64 = 3;
    TEST_ASSERT_FALSE(gw_conv_in_write_limits(&t, &v));
    v.i64 = -3;
    TEST_ASSERT_FALSE(gw_conv_in_write_limits(&t, &v));
    t.write_min = -1e30;                                         /* outside the int64 range */
    t.write_max = 1e30;
    v.i64 = INT64_MAX;
    TEST_ASSERT_TRUE(gw_conv_in_write_limits(&t, &v));
    v.i64 = INT64_MIN;
    TEST_ASSERT_TRUE(gw_conv_in_write_limits(&t, &v));

    t = tag(GW_TYPE_UINT64, GW_WORD_BIG, 1, 0);
    t.has_write_min = t.has_write_max = true;
    t.write_min = 1.5;
    t.write_max = 18446744073709549568.0;                        /* the last double below 2^64 */
    v.u64 = 1;
    TEST_ASSERT_FALSE(gw_conv_in_write_limits(&t, &v));
    v.u64 = 2;
    TEST_ASSERT_TRUE(gw_conv_in_write_limits(&t, &v));
    v.u64 = 18446744073709549568ull;
    TEST_ASSERT_TRUE(gw_conv_in_write_limits(&t, &v));
    v.u64 = 18446744073709549569ull;
    TEST_ASSERT_FALSE(gw_conv_in_write_limits(&t, &v));
    t.write_min = -1;
    t.write_max = 1e30;
    v.u64 = 0;
    TEST_ASSERT_TRUE(gw_conv_in_write_limits(&t, &v));
    v.u64 = UINT64_MAX;
    TEST_ASSERT_TRUE(gw_conv_in_write_limits(&t, &v));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_datatype_mapping);
    RUN_TEST(test_scaled_32bit_keeps_precision);
    RUN_TEST(test_decode_16bit);
    RUN_TEST(test_decode_32bit_word_order);
    RUN_TEST(test_encode_inverse_scale);
    RUN_TEST(test_encode_32bit_round_trip);
    RUN_TEST(test_datatype_mapping_mb6);
    RUN_TEST(test_byte_orders_32bit);
    RUN_TEST(test_64bit_exact_and_orders);
    RUN_TEST(test_float64);
    RUN_TEST(test_strings);
    RUN_TEST(test_bytes);
    RUN_TEST(test_write_limits);
    return UNITY_END();
}
