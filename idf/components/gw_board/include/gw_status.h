/* Gateway status flags and the RGB status LED (IO-5).
 *
 * LED patterns, highest priority first:
 *   BOOT held for a reset  white, fast blink (4 Hz) from 2 s; steady white at the reset
 *   identify (#207)        red, green and blue in turn at 4 Hz, for 10 s (POST /api/identify,
 *                          the shell's identify)
 *   claim window open, or  blue and white, alternating every 250 ms (issue #120: the web
 *   access point open      password can be set from the access point without a credential;
 *                          issue #187: the access point has no key)
 *   commissioning         blue, slow blink (1 Hz): no configuration after a factory reset
 *   configuration error    red, fast blink (4 Hz)
 *   no network (no IP)     red, steady
 *   no valid clock         magenta, slow blink (1 Hz): issue #160, no MQTT session before a
 *                          valid clock (SNTP, the RTC or the browser)
 *   no MQTT session        amber, slow blink (1 Hz)
 *   Modbus device offline  green / orange alternating (1 Hz)
 *   operating              green, steady
 * Issue #187: while the access point runs with WPA2, a short cyan flash (125 ms) every 2 s
 * replaces the pattern below it (from "commissioning" down), which stays visible between the
 * flashes.
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
#define GW_ST_CLAIM     (1u << 7)   /* CFG-3 / NET-5 claim window open (issue #120) */
#define GW_ST_AP        (1u << 8)   /* NET-5 access point running (issue #187) */
#define GW_ST_AP_OPEN   (1u << 9)   /* ... with no key (the open period of a claim window) */
#define GW_ST_NO_CLOCK  (1u << 10)  /* issue #160: the MQTT session waits for a valid clock */

void gw_status_set(uint32_t flag, bool on);

/** Current GW_ST_* flags. */
uint32_t gw_status_get(void);

/** Start the LED task (low priority). */
esp_err_t gw_status_led_start(void);

/** #207: the LED and the buzzer identify the gateway for 10 s (gw_ident.h). False when an
 *  identify runs: its time does not start again. */
bool gw_status_identify(void);

/** #207: the seconds left of the identify, 0 when none runs. */
uint32_t gw_status_identify_left_s(void);

/** #207: true while the identify runs, with the ms since its start. */
bool gw_status_identify_elapsed(uint32_t *elapsed_ms);

#ifdef __cplusplus
}
#endif
