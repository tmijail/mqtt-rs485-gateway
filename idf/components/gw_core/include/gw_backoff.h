/* Exponential reconnect backoff with jitter (MQTT-2). */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Delay before reconnect attempt number @p attempt (1 = first retry).
 * base = min_ms * 2^(attempt-1) capped at max_ms; the result is drawn from
 * [0.75 * base, base] using @p rnd and clamped to [min_ms, max_ms], so the
 * jitter is still applied once the cap is reached.
 */
uint32_t gw_backoff_ms(uint32_t attempt, uint32_t min_ms, uint32_t max_ms, uint32_t rnd);

#ifdef __cplusplus
}
#endif
