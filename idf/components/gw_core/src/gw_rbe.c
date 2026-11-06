#include "gw_rbe.h"

#include <math.h>

bool gw_rbe_changed(const gw_rbe_t *state, double value, bool valid, double deadband)
{
    if (!state->reported || state->valid != valid) {
        return true;
    }
    if (!valid) {
        return false;
    }
    bool was_nan = isnan(state->value);
    if (was_nan || isnan(value)) {
        return was_nan != (bool)isnan(value);
    }
    if (deadband > 0.0) {
        return fabs(value - state->value) > deadband;
    }
    return value != state->value;
}

void gw_rbe_commit(gw_rbe_t *state, double value, bool valid)
{
    state->value = value;
    state->valid = valid;
    state->reported = true;
}

bool gw_rbe_changed_key(const gw_rbe_t *state, uint64_t key, bool is_signed, bool valid,
                        double deadband)
{
    if (!state->reported || state->valid != valid) {
        return true;
    }
    if (!valid) {
        return false;
    }
    if (deadband > 0.0) {
        bool greater = is_signed ? (int64_t)key > (int64_t)state->key : key > state->key;
        uint64_t distance = greater ? key - state->key : state->key - key;  /* exact, mod 2^64 */
        return (double)distance > deadband;
    }
    return key != state->key;
}

void gw_rbe_commit_key(gw_rbe_t *state, uint64_t key, bool valid)
{
    state->key = key;
    state->valid = valid;
    state->reported = true;
}

uint64_t gw_rbe_hash(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        h = (h ^ *c) * 1099511628211ull;
    }
    return h;
}

uint64_t gw_rbe_hash_bytes(const void *data, size_t len)
{
    uint64_t h = 1469598103934665603ull;
    const unsigned char *c = data;
    for (size_t i = 0; i < len; i++) {
        h = (h ^ c[i]) * 1099511628211ull;
    }
    return h;
}
