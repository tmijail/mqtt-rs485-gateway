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
