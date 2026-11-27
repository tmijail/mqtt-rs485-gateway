#include "gw_button.h"

#include <string.h>

void gw_button_init(gw_button_t *b, uint32_t now_ms)
{
    memset(b, 0, sizeof(*b));
    b->raw_since = now_ms;
    b->raw = true;        /* assume pressed until a stable release is seen */
    b->pressed = true;
}

gw_btn_event_t gw_button_update(gw_button_t *b, bool pressed, uint32_t now_ms)
{
    if (pressed != b->raw) {
        b->raw = pressed;
        b->raw_since = now_ms;
    }
    if (b->raw != b->pressed && now_ms - b->raw_since >= GW_BTN_DEBOUNCE_MS) {
        b->pressed = b->raw;
        if (b->pressed) {
            b->pressed_at = now_ms;
            b->phase = GW_BTN_PHASE_NONE;
            return GW_BTN_NONE;
        }
        /* issue #292: the release decides by the mark that the person heard */
        gw_btn_event_t ev = GW_BTN_NONE;
        if (b->armed && b->phase == GW_BTN_PHASE_RESET) {
            ev = GW_BTN_FACTORY_RESET;
        } else if (b->armed && b->phase == GW_BTN_PHASE_AP) {
            ev = GW_BTN_COMMISSIONING;
        }
        b->phase = GW_BTN_PHASE_NONE;
        b->armed = true;
        return ev;
    }
    /* the marks, one at a time and in order; only while the raw sample is pressed too */
    if (!b->armed || !b->pressed || !b->raw) {
        return GW_BTN_NONE;
    }
    uint32_t held = now_ms - b->pressed_at;
    if (b->phase == GW_BTN_PHASE_NONE && held >= GW_BTN_AP_MS) {
        b->phase = GW_BTN_PHASE_AP;
        return GW_BTN_MARK_AP;
    }
    if (b->phase == GW_BTN_PHASE_AP && held >= GW_BTN_RESET_MS) {
        b->phase = GW_BTN_PHASE_RESET;
        return GW_BTN_MARK_RESET;
    }
    return GW_BTN_NONE;
}

uint32_t gw_button_held_ms(const gw_button_t *b, uint32_t now_ms)
{
    return b->armed && b->pressed ? now_ms - b->pressed_at : 0;
}

gw_btn_phase_t gw_button_phase(const gw_button_t *b)
{
    return b->armed && b->pressed ? (gw_btn_phase_t)b->phase : GW_BTN_PHASE_NONE;
}
