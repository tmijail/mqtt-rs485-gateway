/*
 * Reconnection back-off (MQTT-2): exponential from 1 s to 60 s with jitter.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GW_BACKOFF_H_
#define GW_BACKOFF_H_

#include <stdint.h>

#define BACKOFF_MIN_MS 1000U
#define BACKOFF_MAX_MS 60000U

/**
 * Delay before reconnection attempt number @p attempt (0 = first retry).
 * The ceiling doubles per attempt up to BACKOFF_MAX_MS and the delay is
 * drawn from [ceiling/2, ceiling] using @p rnd ("equal jitter"), never
 * below BACKOFF_MIN_MS.
 */
static inline uint32_t backoff_delay_ms(uint32_t attempt, uint32_t rnd)
{
	uint32_t ceiling = (attempt >= 6) ? BACKOFF_MAX_MS : (BACKOFF_MIN_MS << attempt);
	uint32_t delay;

	if (ceiling > BACKOFF_MAX_MS) {
		ceiling = BACKOFF_MAX_MS;
	}
	delay = ceiling / 2U + rnd % (ceiling / 2U + 1U);
	return (delay < BACKOFF_MIN_MS) ? BACKOFF_MIN_MS : delay;
}

#endif /* GW_BACKOFF_H_ */
