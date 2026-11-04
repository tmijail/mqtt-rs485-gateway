/*
 * BOOT button (GPIO0) gestures, from periodic samples (CFG-5, NET-5). Pure logic:
 *
 *   held >= GW_BTN_RESET_MS (10 s)              FACTORY_RESET, at the 10 s mark, once
 *   released after GW_BTN_AP_MS..10 s (2-10 s)   COMMISSIONING (start the access point)
 *   shorter presses                              nothing (bumps, bounces)
 *
 * GPIO0 is a strapping pin: held at reset it selects the ROM download mode, so the firmware
 * only ever sees it after boot. A press counts only after the button was seen released
 * (debounced) once, so a button stuck or held since power-up never resets the device.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_BTN_DEBOUNCE_MS 50
#define GW_BTN_AP_MS       2000
#define GW_BTN_RESET_MS    10000

typedef enum {
    GW_BTN_NONE = 0,
    GW_BTN_COMMISSIONING,
    GW_BTN_FACTORY_RESET,
} gw_btn_event_t;

typedef struct {
    bool raw;             /* last sample */
    uint32_t raw_since;   /* time of the last raw change */
    bool pressed;         /* debounced state */
    uint32_t pressed_at;
    bool armed;           /* released state seen since start */
    bool reset_fired;     /* FACTORY_RESET already reported for this press */
} gw_button_t;

void gw_button_init(gw_button_t *b, uint32_t now_ms);

/** Feed one sample (@p pressed = button down) taken at @p now_ms; returns the gesture. */
gw_btn_event_t gw_button_update(gw_button_t *b, bool pressed, uint32_t now_ms);

/** How long the current press has lasted (0 when released or not armed). */
uint32_t gw_button_held_ms(const gw_button_t *b, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
