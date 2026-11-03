/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "debounce.h"

#include <string.h>

void debounce_init(struct debounce *db, uint8_t initial, uint16_t samples)
{
	memset(db, 0, sizeof(*db));
	db->state = initial;
	db->samples = (samples == 0) ? 1 : samples;
}

uint8_t debounce_step(struct debounce *db, uint8_t raw)
{
	uint8_t diff = raw ^ db->state;
	uint8_t flipped = 0;

	for (int i = 0; i < 8; i++) {
		if ((diff & (1U << i)) == 0) {
			db->count[i] = 0;
		} else if (++db->count[i] >= db->samples) {
			db->count[i] = 0;
			flipped |= (uint8_t)(1U << i);
		}
	}
	db->state ^= flipped;
	return flipped;
}
