/*
 * Digital input debouncing (IO-1).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>

#include "debounce.h"

static uint8_t feed(struct debounce *db, uint8_t raw, int samples)
{
	uint8_t flipped = 0;

	for (int i = 0; i < samples; i++) {
		flipped |= debounce_step(db, raw);
	}
	return flipped;
}

ZTEST(debounce, test_glitch_rejected)
{
	struct debounce db;

	debounce_init(&db, 0x00, 20);
	zassert_equal(feed(&db, 0x04, 19), 0, "19 ms glitch must be ignored");
	zassert_equal(feed(&db, 0x00, 1), 0);
	zassert_equal(db.state, 0x00);
}

ZTEST(debounce, test_stable_change_accepted)
{
	struct debounce db;

	debounce_init(&db, 0x00, 20);
	zassert_equal(feed(&db, 0x04, 19), 0);
	zassert_equal(debounce_step(&db, 0x04), 0x04, "accepted on the 20th sample");
	zassert_equal(db.state, 0x04);
	zassert_equal(feed(&db, 0x04, 100), 0, "no repeated events");
	zassert_equal(feed(&db, 0x00, 20), 0x04);
	zassert_equal(db.state, 0x00);
}

ZTEST(debounce, test_channels_independent)
{
	struct debounce db;

	debounce_init(&db, 0xF0, 5);
	zassert_equal(feed(&db, 0xF1, 3), 0);
	zassert_equal(feed(&db, 0x71, 2), 0x01, "bit 0 completes, bit 7 just started");
	zassert_equal(feed(&db, 0x71, 3), 0x80);
	zassert_equal(db.state, 0x71);
}

ZTEST(debounce, test_zero_means_immediate)
{
	struct debounce db;

	debounce_init(&db, 0x00, 0);
	zassert_equal(debounce_step(&db, 0xFF), 0xFF);
	zassert_equal(debounce_step(&db, 0x00), 0xFF);
}

ZTEST_SUITE(debounce, NULL, NULL, NULL, NULL, NULL);
