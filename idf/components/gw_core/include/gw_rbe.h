/*
 * Report by exception with absolute deadband (SPB-7, SPB-12).
 *
 * The reference is the last *reported* value, so slow drifts are still reported
 * once they exceed the deadband.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double value;
    bool valid;
    bool reported;   /* false until the first report (birth) */
} gw_rbe_t;

/** true if (@p value, @p valid) must be reported. A deadband <= 0 means "any change". */
bool gw_rbe_changed(const gw_rbe_t *state, double value, bool valid, double deadband);

/** Record that (@p value, @p valid) has been reported. */
void gw_rbe_commit(gw_rbe_t *state, double value, bool valid);

#ifdef __cplusplus
}
#endif
