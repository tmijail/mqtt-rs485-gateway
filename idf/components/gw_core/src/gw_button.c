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
    gw_btn_event_t ev = GW_BTN_NONE;
    if (b->raw != b->pressed && now_ms - b->raw_since >= GW_BTN_DEBOUNCE_MS) {
        b->pressed = b->raw;
        if (b->pressed) {
            b->pressed_at = now_ms;
            b->reset_fired = false;
        } else {
            uint32_t held = now_ms - b->pressed_at;
            if (b->armed && !b->reset_fired && held >= GW_BTN_AP_MS) {
                ev = GW_BTN_COMMISSIONING;
            }
            b->armed = true;
        }
    }
    if (b->armed && b->pressed && !b->reset_fired && now_ms - b->pressed_at >= GW_BTN_RESET_MS) {
        b->reset_fired = true;
        ev = GW_BTN_FACTORY_RESET;
    }
    return ev;
}

uint32_t gw_button_held_ms(const gw_button_t *b, uint32_t now_ms)
{
    return b->armed && b->pressed ? now_ms - b->pressed_at : 0;
}
