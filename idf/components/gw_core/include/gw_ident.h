/*
 * Issue #207: "Identify" (POST /api/identify, the shell command identify). Pure C (host-tested).
 *
 * For GW_IDENT_MS after a request, the status LED shows red, green and blue in turn, one color
 * every GW_IDENT_STEP_MS (4 Hz), and the buzzer gives short beeps: GW_IDENT_TONE_MS of tone at
 * the start of each GW_IDENT_BEEP_MS. A request while the identify runs is refused: it does not
 * start the time again (the web UI keeps its button disabled for the time left).
 *
 * The times are in ms of a 32-bit monotonic clock: the arithmetic is safe across its wrap.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_IDENT_MS       10000u
#define GW_IDENT_STEP_MS  250u     /* one color: red, green, blue in turn at 4 Hz */
#define GW_IDENT_BEEP_MS  500u     /* one beep in each period */
#define GW_IDENT_TONE_MS  150u     /* the tone at the start of the period */

typedef enum { GW_IDENT_RED = 0, GW_IDENT_GREEN, GW_IDENT_BLUE } gw_ident_color_t;

/** The ms left of an identify that ends at @p until_ms (0: none ever started), at @p now_ms;
 *  0 when it is over. */
uint32_t gw_ident_left_ms(uint32_t until_ms, uint32_t now_ms);

/** The seconds left, rounded up (the web UI and the shell show them). */
uint32_t gw_ident_left_s(uint32_t until_ms, uint32_t now_ms);

/** The end of a new identify that starts at @p now_ms, or 0 when one runs (refused). Never 0
 *  for a new one. */
uint32_t gw_ident_start(uint32_t until_ms, uint32_t now_ms);

/** The color of the LED @p elapsed_ms after the start. */
gw_ident_color_t gw_ident_color(uint32_t elapsed_ms);

/** The buzzer tone @p elapsed_ms after the start. */
bool gw_ident_tone(uint32_t elapsed_ms);

#ifdef __cplusplus
}
#endif
