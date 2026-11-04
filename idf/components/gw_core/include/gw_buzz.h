/*
 * Buzzer patterns and alarms (IO-6). Pure logic: gw_board feeds the status flags and the
 * clock and turns the tone on and off.
 *
 *   beep          150 ms tone, 150 ms silence
 *   request       N beeps at once (factory reset confirmation: 2; access point gesture: 1)
 *   alarm         N beeps every 10 s while it lasts; N = its gw_alarm_t value:
 *                 device offline 1, MQTT down 2, configuration error 3. Only enabled alarms
 *                 sound, the highest wins; MQTT down and device offline only after their
 *                 condition has lasted alarm_after_s (configuration error at once).
 * A request plays after the burst in progress; an alarm burst never cuts a request.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_BUZZ_ON_MS      150u
#define GW_BUZZ_OFF_MS     150u
#define GW_BUZZ_PERIOD_MS  10000u
#define GW_BUZZ_MAX_BEEPS  9

typedef struct {
    uint8_t enabled;            /* GW_ALARM_BIT() mask */
    uint32_t after_ms;
    bool cond[4];               /* condition of each gw_alarm_t seen at the last update */
    uint32_t since[4];          /* when it started */
    gw_alarm_t alarm;           /* sounding alarm */
    uint8_t request;            /* beeps requested, not started yet */
    uint8_t left;               /* beeps left in the burst being played */
    bool tone;
    uint32_t step_at;           /* next tone change of the burst */
    uint32_t burst_at;          /* next alarm burst */
} gw_buzz_t;

void gw_buzz_init(gw_buzz_t *b, uint8_t enabled_alarms, uint32_t after_s);

/** Update the alarm from the conditions at @p now_ms; returns the alarm that sounds now. */
gw_alarm_t gw_buzz_update(gw_buzz_t *b, bool config_error, bool mqtt_down, bool device_offline,
                          uint32_t now_ms);

/** Queue @p beeps (1..GW_BUZZ_MAX_BEEPS) beeps. */
void gw_buzz_request(gw_buzz_t *b, uint8_t beeps);

/** Tone on or off at @p now_ms (call at least every 50 ms). */
bool gw_buzz_tone(gw_buzz_t *b, uint32_t now_ms);

/** Nothing playing nor queued. */
bool gw_buzz_idle(const gw_buzz_t *b);

/** "device offline", "MQTT down", "configuration error" or "none". */
const char *gw_alarm_name(gw_alarm_t a);

#ifdef __cplusplus
}
#endif
