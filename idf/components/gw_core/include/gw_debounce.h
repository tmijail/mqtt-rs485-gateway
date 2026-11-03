/*
 * Debounce for the 8 digital inputs (IO-1): a level is accepted after it has
 * been sampled identically for `threshold` consecutive samples.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t stable;          /* debounced state, bit n = DI n+1 */
    uint16_t threshold;      /* consecutive samples needed to accept a change (>= 1) */
    uint16_t count[8];
} gw_debounce_t;

void gw_debounce_init(gw_debounce_t *db, uint16_t threshold, uint8_t initial);

/** Feed one raw sample; returns the mask of inputs whose debounced state changed. */
uint8_t gw_debounce_sample(gw_debounce_t *db, uint8_t raw);

#ifdef __cplusplus
}
#endif
