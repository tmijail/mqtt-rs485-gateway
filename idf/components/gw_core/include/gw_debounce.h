/*
 * Debounce for the 8 digital inputs (IO-1): a level is accepted after it has
 * been sampled identically for `threshold` consecutive samples. #211: each input has its own
 * threshold, and an input can be inverted before the debounce (local_io.di[].invert).
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t stable;          /* debounced state, bit n = DI n+1 (after the inversion) */
    uint8_t invert;          /* #211: inputs inverted before the debounce */
    uint16_t threshold[8];   /* consecutive samples needed to accept a change (>= 1), per input */
    uint16_t count[8];
} gw_debounce_t;

/** The same @p threshold for every input, no inversion; @p initial is the accepted state. */
void gw_debounce_init(gw_debounce_t *db, uint16_t threshold, uint8_t initial);

/** #211: a threshold and the inversion per input; @p raw is the first raw sample (before the
 *  inversion): the accepted state starts at raw ^ invert. */
void gw_debounce_init_each(gw_debounce_t *db, const uint16_t threshold[8], uint8_t invert,
                           uint8_t raw);

/** Samples of @p sample_us for a debounce of @p ms (0 ms: 1 sample, each change at once). */
uint16_t gw_debounce_samples(uint16_t ms, uint32_t sample_us);

/** Feed one raw sample (before the inversion); returns the mask of inputs whose debounced
 *  state changed. */
uint8_t gw_debounce_sample(gw_debounce_t *db, uint8_t raw);

#ifdef __cplusplus
}
#endif
