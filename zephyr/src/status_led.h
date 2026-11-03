/*
 * RGB status LED (IO-5). Priority of the shown condition, highest first:
 *
 *   configuration error   red, fast blink (4 Hz)
 *   no network            red, slow blink (1 Hz)
 *   no MQTT broker        yellow, slow blink (1 Hz)
 *   Modbus device offline orange, steady
 *   operating             green, steady
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GW_STATUS_LED_H_
#define GW_STATUS_LED_H_

#include <stdint.h>

struct status_rgb {
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

/** Colour to show at @p uptime_ms for the gateway status flags @p status. */
struct status_rgb status_led_color(uint32_t status, int64_t uptime_ms);

/** Refresh the LED; call periodically (every 50 ms is enough). */
void status_led_update(int64_t uptime_ms);

#endif /* GW_STATUS_LED_H_ */
