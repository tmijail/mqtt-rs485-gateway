/*
 * Sparkplug B payload codec and topics (SPB-1).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "sparkplug.h"

static const struct spb_metric sample[] = {
	{.name = "b", .has_alias = true, .alias = 1, .has_datatype = true,
	 .datatype = SPB_DT_BOOLEAN, .timestamp = 1700000000000ULL, .v.b = true},
	{.name = "i16", .has_alias = true, .alias = 2, .has_datatype = true,
	 .datatype = SPB_DT_INT16, .v.i = -5},
	{.name = "u32", .has_alias = true, .alias = 3, .has_datatype = true,
	 .datatype = SPB_DT_UINT32, .v.u = 0xFFFFFFF0u},
	{.name = "f", .has_alias = true, .alias = 4, .has_datatype = true,
	 .datatype = SPB_DT_FLOAT, .v.d = 12.3},
	{.name = "d", .has_alias = true, .alias = 5, .has_datatype = true,
	 .datatype = SPB_DT_DOUBLE, .v.d = -1.5e10},
	{.name = "s", .has_alias = true, .alias = 6, .has_datatype = true,
	 .datatype = SPB_DT_STRING, .v.s = "0.1.0+abc"},
	{.name = "u64", .has_alias = true, .alias = 7, .has_datatype = true,
	 .datatype = SPB_DT_UINT64, .v.u = 255},
	{.name = NULL, .has_alias = true, .alias = 1000, .datatype = SPB_DT_FLOAT, .v.d = 1.0},
	{.name = "null", .has_alias = true, .alias = 8, .has_datatype = true,
	 .datatype = SPB_DT_INT32, .is_null = true},
};

static int get_sample(void *ctx, size_t i, struct spb_metric *m)
{
	ARG_UNUSED(ctx);
	*m = sample[i];
	return 0;
}

static struct spb_metric_in decoded[ARRAY_SIZE(sample) + 1];
static size_t n_decoded;

static void collect(void *ctx, const struct spb_metric_in *m)
{
	ARG_UNUSED(ctx);
	if (n_decoded < ARRAY_SIZE(decoded)) {
		decoded[n_decoded] = *m;
	}
	n_decoded++;
}

static void before(void *f)
{
	ARG_UNUSED(f);
	n_decoded = 0;
	memset(decoded, 0, sizeof(decoded));
}

ZTEST(sparkplug, test_round_trip)
{
	struct spb_payload_out p = {.timestamp = 1700000000123ULL, .has_seq = true, .seq = 42,
				    .metric_count = ARRAY_SIZE(sample), .get = get_sample};
	uint8_t buf[512];
	size_t len;
	double v;

	zassert_ok(spb_encode(&p, buf, sizeof(buf), &len));
	zassert_ok(spb_decode(buf, len, collect, NULL));
	zassert_equal(n_decoded, ARRAY_SIZE(sample));

	zassert_str_equal(decoded[0].name, "b");
	zassert_equal(decoded[0].kind, SPB_VAL_BOOL);
	zassert_true(decoded[0].v.b);
	zassert_equal(decoded[0].datatype, SPB_DT_BOOLEAN);

	/* Int16 travels as the 32-bit two's complement in int_value. */
	zassert_equal(decoded[1].kind, SPB_VAL_INT);
	zassert_equal(decoded[1].v.i, 0xFFFFFFFBu);
	zassert_ok(spb_metric_in_number(&decoded[1], SPB_DT_INT16, &v));
	zassert_equal(v, -5.0);

	zassert_ok(spb_metric_in_number(&decoded[2], SPB_DT_UINT32, &v));
	zassert_equal(v, (double)0xFFFFFFF0u);
	zassert_equal(decoded[3].kind, SPB_VAL_FLOAT);
	zassert_within((double)decoded[3].v.f, 12.3, 1e-6);
	zassert_equal(decoded[4].kind, SPB_VAL_DOUBLE);
	zassert_equal(decoded[4].v.d, -1.5e10);
	zassert_equal(decoded[5].kind, SPB_VAL_STRING);
	zassert_str_equal(decoded[5].v.s, "0.1.0+abc");
	zassert_equal(decoded[6].kind, SPB_VAL_LONG);
	zassert_equal(decoded[6].v.l, 255);

	zassert_false(decoded[7].has_name, "DATA metrics travel by alias only");
	zassert_false(decoded[7].has_datatype);
	zassert_equal(decoded[7].alias, 1000);

	zassert_true(decoded[8].is_null);
	zassert_equal(decoded[8].kind, SPB_VAL_NONE);
	zassert_equal(spb_metric_in_number(&decoded[8], SPB_DT_INT32, &v), -EINVAL);
}

static int get_ctx(void *ctx, size_t i, struct spb_metric *m)
{
	ARG_UNUSED(i);
	*m = *(const struct spb_metric *)ctx;
	return 0;
}

ZTEST(sparkplug, test_encode_limits)
{
	static const struct spb_metric long_name = {
		.name = "0123456789012345678901234567890123456789012345678901234567890123",
		.datatype = SPB_DT_BOOLEAN};
	static const struct spb_metric long_string = {
		.name = "s", .datatype = SPB_DT_STRING,
		.v.s = "0123456789012345678901234567890123456789012345678901234567890123"};
	struct spb_payload_out p = {.has_seq = true, .metric_count = ARRAY_SIZE(sample),
				    .get = get_sample};
	uint8_t buf[512];
	size_t len;

	zassert_equal(spb_encode(&p, buf, 40, &len), -ENOMEM, "buffer too small");

	p.metric_count = 0;
	zassert_ok(spb_encode(&p, buf, sizeof(buf), &len), "payload without metrics (DDEATH)");

	p = (struct spb_payload_out){.metric_count = 1, .get = get_ctx, .ctx = (void *)&long_name};
	zassert_equal(spb_encode(&p, buf, sizeof(buf), &len), -EINVAL, "64-byte name");
	p.ctx = (void *)&long_string;
	zassert_equal(spb_encode(&p, buf, sizeof(buf), &len), -EINVAL, "64-byte string");
}

ZTEST(sparkplug, test_decode_malformed)
{
	static const uint8_t garbage[] = {0x12, 0x40, 0x0a, 0x01}; /* truncated metric */

	zassert_equal(spb_decode(garbage, sizeof(garbage), collect, NULL), -EINVAL);
}

ZTEST(sparkplug, test_topics)
{
	char buf[64];
	char dev[33];

	zassert_true(spb_topic(buf, sizeof(buf), "Plant1", "NBIRTH", "GW01", NULL) > 0);
	zassert_str_equal(buf, "spBv1.0/Plant1/NBIRTH/GW01");
	zassert_true(spb_topic(buf, sizeof(buf), "Plant1", "DDATA", "GW01", "Meter1") > 0);
	zassert_str_equal(buf, "spBv1.0/Plant1/DDATA/GW01/Meter1");
	zassert_equal(spb_topic(buf, 10, "Plant1", "DDATA", "GW01", "Meter1"), -ENOMEM);

#define PARSE(t) spb_topic_parse_cmd(t, strlen(t), "Plant1", "GW01", dev, sizeof(dev))
	zassert_equal(PARSE("spBv1.0/Plant1/NCMD/GW01"), SPB_CMD_NODE);
	zassert_equal(PARSE("spBv1.0/Plant1/DCMD/GW01/Meter1"), SPB_CMD_DEVICE);
	zassert_str_equal(dev, "Meter1");
	zassert_equal(PARSE("spBv1.0/Plant1/NCMD/GW01/"), -EINVAL);
	zassert_equal(PARSE("spBv1.0/Plant1/NCMD/GW011"), -EINVAL);
	zassert_equal(PARSE("spBv1.0/Plant1/NCMD/GW0"), -EINVAL);
	zassert_equal(PARSE("spBv1.0/Plant2/NCMD/GW01"), -EINVAL);
	zassert_equal(PARSE("spBv1.0/Plant1/NDATA/GW01"), -EINVAL);
	zassert_equal(PARSE("spBv1.0/Plant1/DCMD/GW01"), -EINVAL);
	zassert_equal(PARSE("spBv1.0/Plant1/DCMD/GW01/"), -EINVAL);
	zassert_equal(PARSE("spBv1.0/Plant1/DCMD/GW01/a/b"), -EINVAL);
	zassert_equal(PARSE("spBv1.0/STATE/host"), -EINVAL);
#undef PARSE
}

ZTEST_SUITE(sparkplug, NULL, NULL, before, NULL, NULL);
