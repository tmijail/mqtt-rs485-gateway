/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "status_led.h"

#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "model.h"

LOG_MODULE_REGISTER(status_led, CONFIG_GW_LOG_LEVEL);

#define LEVEL 40 /* keep the LED readable without being blinding */

static const struct status_rgb OFF = {0, 0, 0};
static const struct status_rgb RED = {LEVEL, 0, 0};
static const struct status_rgb YELLOW = {LEVEL, LEVEL, 0};
static const struct status_rgb ORANGE = {LEVEL, LEVEL / 4, 0};
static const struct status_rgb GREEN = {0, LEVEL, 0};

static struct status_rgb blink(struct status_rgb on, int64_t uptime_ms, int64_t period_ms)
{
	return ((uptime_ms % period_ms) < period_ms / 2) ? on : OFF;
}

struct status_rgb status_led_color(uint32_t status, int64_t uptime_ms)
{
	if (status & GW_STATUS_CONFIG_ERROR) {
		return blink(RED, uptime_ms, 250);
	}
	if (!(status & GW_STATUS_NET_UP)) {
		return blink(RED, uptime_ms, 1000);
	}
	if (!(status & GW_STATUS_MQTT_UP)) {
		return blink(YELLOW, uptime_ms, 1000);
	}
	if (status & GW_STATUS_MODBUS_ERROR) {
		return ORANGE;
	}
	return GREEN;
}

#if defined(CONFIG_GW_STATUS_LED) && DT_HAS_ALIAS(status_led)

static const struct device *const strip = DEVICE_DT_GET(DT_ALIAS(status_led));

void status_led_update(int64_t uptime_ms)
{
	static struct status_rgb shown = {1, 1, 1}; /* force the first write */
	static bool ready_checked;
	struct status_rgb c = status_led_color(model_status(), uptime_ms);
	struct led_rgb px;

	if (!ready_checked) {
		ready_checked = true;
		if (!device_is_ready(strip)) {
			LOG_ERR("status LED not ready");
		}
	}
	if (!device_is_ready(strip) ||
	    (c.r == shown.r && c.g == shown.g && c.b == shown.b)) {
		return;
	}
	px = (struct led_rgb){.r = c.r, .g = c.g, .b = c.b};
	if (led_strip_update_rgb(strip, &px, 1) == 0) {
		shown = c;
	}
}

#else

void status_led_update(int64_t uptime_ms)
{
	ARG_UNUSED(uptime_ms);
}

#endif
