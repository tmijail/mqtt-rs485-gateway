/*
 * Report by exception and deadband (SPB-7, SPB-12).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>

#include <zephyr/ztest.h>

#include "model.h"
#include "test_util.h"

ZTEST(rbe, test_exceeds)
{
	struct gw_tag_cfg analog = {.type = GW_TYPE_FLOAT32, .scale = 1, .deadband = 0.1};
	struct gw_tag_cfg plain = {.type = GW_TYPE_UINT16, .scale = 1};
	struct gw_tag_cfg bit = {.type = GW_TYPE_BOOL, .scale = 1, .deadband = 5};

	zassert_false(model_rbe_exceeds(&analog, 230.0, 230.05));
	zassert_false(model_rbe_exceeds(&analog, 230.0, 230.0));
	zassert_false(model_rbe_exceeds(&analog, 230.0, 230.0 + 0.1), "equal to the band");
	zassert_true(model_rbe_exceeds(&analog, 230.0, 230.11));
	zassert_true(model_rbe_exceeds(&analog, 230.0, 229.8), "both directions");
	zassert_true(model_rbe_exceeds(&analog, 230.0, NAN));
	zassert_true(model_rbe_exceeds(&analog, NAN, 230.0));
	zassert_false(model_rbe_exceeds(&analog, NAN, NAN));

	zassert_false(model_rbe_exceeds(&plain, 7, 7));
	zassert_true(model_rbe_exceeds(&plain, 7, 8), "no deadband: any change");
	zassert_true(model_rbe_exceeds(&bit, 0, 1), "deadband ignored for bool");
}

static void *setup(void)
{
	return NULL;
}

static void before(void *f)
{
	ARG_UNUSED(f);
	model_init(test_config_load(spec_config_json));
}

ZTEST(rbe_store, test_pending_tracking)
{
	struct gw_tag_state *st = model_tag(0);

	model_tag_store(0, 230.0, 1, false);
	zassert_true(st->pending, "first value is always reported");
	/* What spb_node does when it publishes: */
	st->reported = st->value;
	st->pending = false;

	model_tag_store(0, 230.05, 2, false);
	zassert_false(st->pending, "inside the deadband");
	model_tag_store(0, 230.08, 3, false);
	zassert_false(st->pending, "drift is measured from the last report");
	model_tag_store(0, 230.2, 4, false);
	zassert_true(st->pending);
	zassert_equal(st->ts_ms, 4);
	zassert_within(st->value, 230.2, 1e-12);

	st->reported = st->value;
	st->pending = false;
	model_tag_store(0, 230.2, 5, true);
	zassert_true(st->pending, "write confirmations are always reported");
}

ZTEST_SUITE(rbe, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(rbe_store, NULL, setup, before, NULL, NULL);
