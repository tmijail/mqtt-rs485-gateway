#include "gw_backoff.h"

uint32_t gw_backoff_ms(uint32_t attempt, uint32_t min_ms, uint32_t max_ms, uint32_t rnd)
{
    uint64_t base = min_ms;
    for (uint32_t i = 1; i < attempt && base < max_ms; i++) {
        base *= 2u;
    }
    if (base > max_ms) {
        base = max_ms;
    }
    uint64_t delay = base - (rnd % (base / 4u + 1u));
    if (delay < min_ms) {
        delay = min_ms;
    }
    return (uint32_t)delay;
}
