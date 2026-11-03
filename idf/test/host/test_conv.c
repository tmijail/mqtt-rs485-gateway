/* QA-1: type and scale conversion (MB-5, MB-7) and the Sparkplug datatype mapping. */
#include <math.h>
#include <string.h>

#include "gw_conv.h"
#include "spb.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

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
    t = tag(GW_TYPE_UINT32, GW_WORD_BIG, 1, -5);   /* offset -> Float */
    TEST_ASSERT_EQUAL(SPB_DT_FLOAT, gw_conv_datatype(&t));
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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_datatype_mapping);
    RUN_TEST(test_decode_16bit);
    RUN_TEST(test_decode_32bit_word_order);
    RUN_TEST(test_encode_inverse_scale);
    RUN_TEST(test_encode_32bit_round_trip);
    return UNITY_END();
}
