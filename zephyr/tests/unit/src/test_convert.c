/*
 * Register/value conversion, word order and scaling (MB-5, MB-7) and the
 * Sparkplug datatype mapping of SPEC section 6.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <math.h>

#include <zephyr/ztest.h>

#include "convert.h"
#include "sparkplug.h"

static struct gw_tag_cfg tag(enum gw_area area, enum gw_type type, enum gw_word_order wo,
			     double scale, double offset)
{
	return (struct gw_tag_cfg){
		.area = area, .type = type, .word_order = wo, .scale = scale, .offset = offset};
}

ZTEST(convert, test_decode_16bit)
{
	struct gw_tag_cfg t = tag(GW_AREA_HOLDING, GW_TYPE_INT16, GW_WORD_BIG, 1, 0);

	zassert_equal(gw_regs_to_value(&t, (uint16_t[]){0xFFFE, 0}), -2.0);
	t.type = GW_TYPE_UINT16;
	zassert_equal(gw_regs_to_value(&t, (uint16_t[]){0xFFFE, 0}), 65534.0);
	t = tag(GW_AREA_COIL, GW_TYPE_BOOL, GW_WORD_BIG, 1, 0);
	zassert_equal(gw_regs_to_value(&t, (uint16_t[]){1, 0}), 1.0);
}

ZTEST(convert, test_decode_32bit_word_order)
{
	struct gw_tag_cfg t = tag(GW_AREA_INPUT, GW_TYPE_UINT32, GW_WORD_BIG, 1, 0);

	zassert_equal(gw_regs_to_value(&t, (uint16_t[]){0x1234, 0x5678}), (double)0x12345678);
	t.word_order = GW_WORD_LITTLE;
	zassert_equal(gw_regs_to_value(&t, (uint16_t[]){0x5678, 0x1234}), (double)0x12345678);

	t.type = GW_TYPE_INT32;
	zassert_equal(gw_regs_to_value(&t, (uint16_t[]){0xFFFF, 0xFFFF}), -1.0);
	zassert_equal(gw_regs_to_value(&t, (uint16_t[]){0x0000, 0x8000}), -2147483648.0);

	t = tag(GW_AREA_INPUT, GW_TYPE_FLOAT32, GW_WORD_BIG, 1, 0);
	zassert_equal(gw_regs_to_value(&t, (uint16_t[]){0x4366, 0x8000}), 230.5);
	t.word_order = GW_WORD_LITTLE;
	zassert_equal(gw_regs_to_value(&t, (uint16_t[]){0x8000, 0x4366}), 230.5);
}

ZTEST(convert, test_scale_offset)
{
	struct gw_tag_cfg t = tag(GW_AREA_HOLDING, GW_TYPE_INT16, GW_WORD_BIG, 0.1, 0);

	zassert_within(gw_regs_to_value(&t, (uint16_t[]){123, 0}), 12.3, 1e-9);
	zassert_within(gw_regs_to_value(&t, (uint16_t[]){(uint16_t)-123, 0}), -12.3, 1e-9);
	t = tag(GW_AREA_INPUT, GW_TYPE_UINT16, GW_WORD_BIG, 0.5, -40);
	zassert_within(gw_regs_to_value(&t, (uint16_t[]){100, 0}), 10.0, 1e-9);
}

ZTEST(convert, test_inverse_for_writes)
{
	struct gw_tag_cfg t = tag(GW_AREA_HOLDING, GW_TYPE_INT16, GW_WORD_BIG, 0.1, 0);
	uint16_t regs[2] = {0};

	/* SPEC 7.4: Setpoint=12.3 must write 123 (also from a float32 12.3f). */
	zassert_ok(gw_value_to_regs(&t, 12.3, regs));
	zassert_equal(regs[0], 123);
	zassert_ok(gw_value_to_regs(&t, (double)12.3f, regs));
	zassert_equal(regs[0], 123);
	zassert_ok(gw_value_to_regs(&t, -12.3, regs));
	zassert_equal(regs[0], (uint16_t)-123);
	zassert_equal(gw_value_to_regs(&t, 3276.8, regs), -ERANGE);
	zassert_equal(gw_value_to_regs(&t, NAN, regs), -ERANGE);

	t = tag(GW_AREA_HOLDING, GW_TYPE_UINT16, GW_WORD_BIG, 1, 0);
	zassert_equal(gw_value_to_regs(&t, -1, regs), -ERANGE);
	zassert_ok(gw_value_to_regs(&t, 65535, regs));
	zassert_equal(regs[0], 65535);

	t = tag(GW_AREA_HOLDING, GW_TYPE_UINT32, GW_WORD_LITTLE, 1, 0);
	zassert_ok(gw_value_to_regs(&t, (double)0x12345678, regs));
	zassert_equal(regs[0], 0x5678);
	zassert_equal(regs[1], 0x1234);

	t = tag(GW_AREA_HOLDING, GW_TYPE_INT32, GW_WORD_BIG, 1, 0);
	zassert_ok(gw_value_to_regs(&t, -2, regs));
	zassert_equal(regs[0], 0xFFFF);
	zassert_equal(regs[1], 0xFFFE);
	zassert_equal(gw_value_to_regs(&t, 2147483648.0, regs), -ERANGE);

	t = tag(GW_AREA_HOLDING, GW_TYPE_FLOAT32, GW_WORD_BIG, 2, 1);
	zassert_ok(gw_value_to_regs(&t, 462.0, regs)); /* raw 230.5 */
	zassert_equal(regs[0], 0x4366);
	zassert_equal(regs[1], 0x8000);
	zassert_equal(gw_value_to_regs(&t, 1e300, regs), -ERANGE);

	t = tag(GW_AREA_COIL, GW_TYPE_BOOL, GW_WORD_BIG, 1, 0);
	zassert_ok(gw_value_to_regs(&t, 1.0, regs));
	zassert_equal(regs[0], 1);
	zassert_ok(gw_value_to_regs(&t, 0.0, regs));
	zassert_equal(regs[0], 0);
}

ZTEST(convert, test_round_trip)
{
	static const enum gw_type types[] = {GW_TYPE_UINT16, GW_TYPE_INT16, GW_TYPE_UINT32,
					     GW_TYPE_INT32, GW_TYPE_FLOAT32};
	static const double values[] = {0, 1, 100, 1234.5, -7.25};

	for (size_t i = 0; i < ARRAY_SIZE(types); i++) {
		for (int wo = GW_WORD_BIG; wo <= GW_WORD_LITTLE; wo++) {
			struct gw_tag_cfg t = tag(GW_AREA_HOLDING, types[i], wo, 0.25, 3);

			for (size_t v = 0; v < ARRAY_SIZE(values); v++) {
				uint16_t regs[2];
				bool unsigned_neg = (types[i] == GW_TYPE_UINT16 ||
						     types[i] == GW_TYPE_UINT32) &&
						    values[v] < 3;

				if (unsigned_neg) {
					continue;
				}
				zassert_ok(gw_value_to_regs(&t, values[v], regs));
				zassert_within(gw_regs_to_value(&t, regs), values[v], 1e-9,
					       "type %d wo %d value %f", types[i], wo, values[v]);
			}
		}
	}
}

ZTEST(convert, test_datatype_mapping)
{
	struct gw_tag_cfg t = tag(GW_AREA_COIL, GW_TYPE_BOOL, GW_WORD_BIG, 1, 0);

	zassert_equal(gw_tag_datatype(&t), SPB_DT_BOOLEAN);
	t = tag(GW_AREA_HOLDING, GW_TYPE_INT16, GW_WORD_BIG, 1, 0);
	zassert_equal(gw_tag_datatype(&t), SPB_DT_INT16);
	t.type = GW_TYPE_UINT16;
	zassert_equal(gw_tag_datatype(&t), SPB_DT_UINT16);
	t.type = GW_TYPE_INT32;
	zassert_equal(gw_tag_datatype(&t), SPB_DT_INT32);
	t.type = GW_TYPE_UINT32;
	zassert_equal(gw_tag_datatype(&t), SPB_DT_UINT32);
	t.type = GW_TYPE_FLOAT32;
	zassert_equal(gw_tag_datatype(&t), SPB_DT_FLOAT);
	t = tag(GW_AREA_HOLDING, GW_TYPE_INT16, GW_WORD_BIG, 0.1, 0);
	zassert_equal(gw_tag_datatype(&t), SPB_DT_FLOAT, "scaled -> Float");
	t = tag(GW_AREA_HOLDING, GW_TYPE_UINT32, GW_WORD_BIG, 1, -5);
	zassert_equal(gw_tag_datatype(&t), SPB_DT_FLOAT, "offset -> Float");
}

ZTEST_SUITE(convert, NULL, NULL, NULL, NULL, NULL);
