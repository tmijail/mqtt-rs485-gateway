/* Buzzer on GPIO46 (IO-6): alarm patterns from the status flags (gw_status) and beeps on
 * request (factory reset confirmation, access point gesture). Patterns in gw_buzz.h.
 * GPIO46 is a strapping pin (must be low at reset): it is only configured here, from
 * app_main, never before. A passive buzzer (gw_bsp) gets a LEDC tone, an active one a level. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "gw_config.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the buzzer task (low priority, not watched), silent until configured. */
esp_err_t gw_buzzer_start(void);

/** Alarms that may sound (GW_ALARM_BIT() mask) and how long MQTT down / device offline
 *  must last first. Callable at any time. */
void gw_buzzer_configure(uint8_t alarms, uint32_t after_s);

/** Issue #75: the alarm sounding now (GW_ALARM_NONE: silent) and the enabled ones. */
gw_alarm_t gw_buzzer_alarm(void);
uint8_t gw_buzzer_alarms(void);

/** Play @p beeps beeps (asynchronous). */
void gw_buzzer_beep(uint8_t beeps);

/** Play @p beeps beeps and wait until they are over (at most about beeps x 300 ms): for a
 *  confirmation that precedes a long flash operation (CFG-5 factory reset). */
void gw_buzzer_beep_wait(uint8_t beeps);

#ifdef __cplusplus
}
#endif
