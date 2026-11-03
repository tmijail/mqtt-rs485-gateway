/*
 * Reconnection back-off (MQTT-2) and status LED priorities (IO-5).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>

#include "backoff.h"
#include "model.h"
#include "status_led.h"

ZTEST(backoff, test_bounds)
{
	uint32_t ceiling = 1000;

	for (uint32_t attempt = 0; attempt < 40; attempt++) {
		static const uint32_t rnd[] = {0, 1, 499, 0x9e3779b9, UINT32_MAX};
		uint32_t lo = MAX(ceiling / 2, BACKOFF_MIN_MS);

		for (size_t i = 0; i < ARRAY_SIZE(rnd); i++) {
			uint32_t d = backoff_delay_ms(attempt, rnd[i]);

			zassert_true(d >= lo && d <= ceiling, "attempt %u: %u not in [%u, %u]",
				     attempt, d, lo, ceiling);
		}
		ceiling = MIN(ceiling * 2, BACKOFF_MAX_MS);
	}
	zassert_equal(backoff_delay_ms(0, 0), 1000, "first retry after 1 s");
	zassert_equal(backoff_delay_ms(30, 30000), 60000, "capped at 60 s");
	zassert_equal(backoff_delay_ms(30, 0), 30000, "jitter spans half the ceiling");
}

static bool is(struct status_rgb c, uint8_t r, uint8_t g, uint8_t b)
{
	return c.r == r && c.g == g && c.b == b;
}

ZTEST(status_led, test_priorities)
{
	uint32_t ok = GW_STATUS_NET_UP | GW_STATUS_MQTT_UP;

	zassert_true(is(status_led_color(ok, 0), 0, 40, 0), "operating: green");
	zassert_true(is(status_led_color(ok | GW_STATUS_MODBUS_ERROR, 0), 40, 10, 0),
		     "modbus error: orange");
	zassert_true(is(status_led_color(GW_STATUS_NET_UP, 0), 40, 40, 0), "no broker: yellow");
	zassert_true(is(status_led_color(GW_STATUS_NET_UP, 600), 0, 0, 0), "slow blink");
	zassert_true(is(status_led_color(0, 0), 40, 0, 0), "no network: red");
	zassert_true(is(status_led_color(ok | GW_STATUS_CONFIG_ERROR, 0), 40, 0, 0));
	zassert_true(is(status_led_color(ok | GW_STATUS_CONFIG_ERROR, 130), 0, 0, 0),
		     "config error blinks fast");
}

ZTEST_SUITE(backoff, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(status_led, NULL, NULL, NULL, NULL, NULL);
