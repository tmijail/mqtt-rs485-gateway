/*
 * Sample-based debouncing of the 8 digital inputs (IO-1).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GW_DEBOUNCE_H_
#define GW_DEBOUNCE_H_

#include <stdint.h>

struct debounce {
	uint8_t state;     /* debounced state, bit n = input n */
	uint16_t count[8]; /* consecutive samples disagreeing with @c state */
	uint16_t samples;  /* samples required to accept a new level (>= 1) */
};

/** @p samples consecutive differing samples are needed to flip an input. */
void debounce_init(struct debounce *db, uint8_t initial, uint16_t samples);

/** Feed one sample of all inputs; returns the mask of inputs that flipped. */
uint8_t debounce_step(struct debounce *db, uint8_t raw);

#endif /* GW_DEBOUNCE_H_ */
