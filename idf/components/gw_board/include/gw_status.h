/* Gateway status flags and the RGB status LED (IO-5).
 *
 * LED patterns, highest priority first:
 *   BOOT held for a reset  white, fast blink (4 Hz) from 2 s; steady white at the reset
 *   commissioning          blue, slow blink (1 Hz): no configuration after a factory reset
 *   configuration error    red, fast blink (4 Hz)
 *   no network (no IP)     red, steady
 *   no MQTT session        amber, slow blink (1 Hz)
 *   Modbus device offline  green / orange alternating (1 Hz)
 *   operating              green, steady
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_ST_CFG_ERROR (1u << 0)
#define GW_ST_NET_UP    (1u << 1)
#define GW_ST_MQTT_UP   (1u << 2)
#define GW_ST_MB_FAULT  (1u << 3)
#define GW_ST_BUTTON    (1u << 4)   /* BOOT held long enough to be a reset gesture */
#define GW_ST_RESETTING (1u << 5)   /* factory reset in progress */
#define GW_ST_COMMISSIONING (1u << 6)

void gw_status_set(uint32_t flag, bool on);

/** Current GW_ST_* flags. */
uint32_t gw_status_get(void);

/** Start the LED task (low priority). */
esp_err_t gw_status_led_start(void);

#ifdef __cplusplus
}
#endif
