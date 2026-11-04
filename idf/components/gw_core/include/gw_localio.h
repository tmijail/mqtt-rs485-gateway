/*
 * Policy of the local digital outputs, without the network (pure logic; the owner of the
 * TCA9554 in gw_board applies what it decides).
 *
 * IO-3 safe state, per DO (local_io.do_failsafe: off, on or hold):
 *   - at a restart: "off" and "on" outputs take that state once the configuration is valid;
 *     "hold" outputs keep the state the expander's latch kept across the restart (OFF after
 *     a power-up). With a rejected configuration every output is OFF.
 *   - after local_io.failsafe_s seconds without a Sparkplug session (counted from the boot
 *     or from the loss): "off" and "on" outputs take that state, once; "hold" outputs do not
 *     change. When the session is back the outputs stay as they are until commanded again.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GW_LIO_FS_NONE = 0,
    GW_LIO_FS_TRIP,    /* the session has been down for failsafe_s: apply the safe states */
    GW_LIO_FS_CLEAR,   /* the session is back after a trip */
} gw_lio_fs_event_t;

typedef struct {
    uint8_t safe_on;          /* outputs whose safe state is ON */
    uint8_t safe_off;         /* outputs whose safe state is OFF (the others hold) */
    uint32_t failsafe_ms;     /* 0: an MQTT loss never trips the safe state */
    bool session;             /* last session state seen */
    uint32_t down_since_ms;
    bool tripped;
} gw_lio_t;

/** Outputs to keep across a restart (do_failsafe "hold") for gw_dout_init(). */
uint8_t gw_lio_hold_mask(const gw_config_t *cfg);

/** Start with the session down since @p now_ms (the boot). */
void gw_lio_init(gw_lio_t *l, const gw_config_t *cfg, uint32_t now_ms);

/** Logical DO states right after the restart, from @p held: what the expander drives after
 *  gw_dout_init() with gw_lio_hold_mask() (held outputs kept, the others off). */
uint8_t gw_lio_boot_state(const gw_lio_t *l, uint8_t held);

/**
 * Feed the Sparkplug session state at @p now_ms (monotonic). On GW_LIO_FS_TRIP the outputs
 * of @p mask must take the states in @p value (both 0 when every output holds).
 */
gw_lio_fs_event_t gw_lio_failsafe(gw_lio_t *l, bool session_up, uint32_t now_ms, uint8_t *mask,
                                  uint8_t *value);

#ifdef __cplusplus
}
#endif
