/*
 * BOOT button (GPIO0) gestures, from periodic samples (CFG-5, NET-5). Pure logic.
 *
 * Issue #292: the two time limits are marks while the button is held, and the action comes at
 * the release, so that a person at the panel hears each limit before the action:
 *
 *   held GW_BTN_AP_MS (2 s)                 MARK_AP, once per press (the gateway beeps 1 time)
 *   held GW_BTN_RESET_MS (10 s)             MARK_RESET, once per press (2 beeps, a warning)
 *   released after MARK_AP, before MARK_RESET    COMMISSIONING (start the access point)
 *   released after MARK_RESET                    FACTORY_RESET
 *   shorter presses                              nothing (bumps, bounces)
 *
 * The marks come in this order, each one time per press: MARK_RESET only after MARK_AP. The
 * release decides by the marks given, not by a new count of the time: the action is always the
 * one that the last mark announced. A mark needs the raw sample pressed too, so that a button
 * that went up during the debounce gets no mark (no factory reset for a release at 9.97 s).
 *
 * A button held for a long time after MARK_RESET gives nothing more until its release: there is
 * no cancel time (decision of the product owner, #292). A button stuck down never gives the
 * factory reset, because the action needs the release.
 *
 * GPIO0 is a strapping pin: held at reset it selects the ROM download mode, so the firmware
 * only ever sees it after boot. A press counts only after the button was seen released
 * (debounced) once, so a button stuck or held since power-up gives no mark and no gesture.
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
    GW_BTN_MARK_AP,         /* held 2 s: a release now starts the access point */
    GW_BTN_MARK_RESET,      /* held 10 s: a release now starts the factory reset */
    GW_BTN_COMMISSIONING,   /* released after MARK_AP and before MARK_RESET */
    GW_BTN_FACTORY_RESET,   /* released after MARK_RESET */
} gw_btn_event_t;

/** The mark of the current press (what a release does now). */
typedef enum {
    GW_BTN_PHASE_NONE = 0,   /* released, not armed, or held less than 2 s */
    GW_BTN_PHASE_AP,         /* MARK_AP given */
    GW_BTN_PHASE_RESET,      /* MARK_RESET given */
} gw_btn_phase_t;

typedef struct {
    bool raw;             /* last sample */
    uint32_t raw_since;   /* time of the last raw change */
    bool pressed;         /* debounced state */
    uint32_t pressed_at;
    bool armed;           /* released state seen since start */
    uint8_t phase;        /* gw_btn_phase_t of the current press */
} gw_button_t;

void gw_button_init(gw_button_t *b, uint32_t now_ms);

/** Feed one sample (@p pressed = button down) taken at @p now_ms; returns the gesture. */
gw_btn_event_t gw_button_update(gw_button_t *b, bool pressed, uint32_t now_ms);

/** How long the current press has lasted (0 when released or not armed). */
uint32_t gw_button_held_ms(const gw_button_t *b, uint32_t now_ms);

/** The mark of the current press: GW_BTN_PHASE_NONE when the button is released. */
gw_btn_phase_t gw_button_phase(const gw_button_t *b);

#ifdef __cplusplus
}
#endif
