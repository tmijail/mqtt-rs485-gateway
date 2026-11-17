#include "gw_ident.h"

uint32_t gw_ident_left_ms(uint32_t until_ms, uint32_t now_ms)
{
    if (until_ms == 0) {
        return 0;
    }
    int32_t left = (int32_t)(until_ms - now_ms);
    /* more than the full time left: a stale end from before a wrap of the clock */
    return left > 0 && (uint32_t)left <= GW_IDENT_MS ? (uint32_t)left : 0;
}

uint32_t gw_ident_left_s(uint32_t until_ms, uint32_t now_ms)
{
    return (gw_ident_left_ms(until_ms, now_ms) + 999u) / 1000u;
}

uint32_t gw_ident_start(uint32_t until_ms, uint32_t now_ms)
{
    if (gw_ident_left_ms(until_ms, now_ms) != 0) {
        return 0;
    }
    uint32_t end = now_ms + GW_IDENT_MS;
    return end != 0 ? end : 1u;
}

gw_ident_color_t gw_ident_color(uint32_t elapsed_ms)
{
    return (gw_ident_color_t)((elapsed_ms / GW_IDENT_STEP_MS) % 3u);
}

bool gw_ident_tone(uint32_t elapsed_ms)
{
    return elapsed_ms % GW_IDENT_BEEP_MS < GW_IDENT_TONE_MS;
}
