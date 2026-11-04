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
 *
 * IO-7 local rules DI -> DO (local_io.rules), evaluated without the network: "follow"
 * (DO = DI), "invert" (DO = not DI) and "toggle" (each activation of the DI toggles the
 * DO; activations come from the IO-4 counters, so none is missed between two evaluations).
 * A DO with a rule belongs to it: NCMD on it is refused and the safe state of an MQTT loss
 * does not touch it (the rule keeps working). At a restart it starts like any DO (do_failsafe
 * "off" or "hold") until the rule is evaluated with the DIs of the boot.
 *
 * IO-4 pulse counters: saved every local_io.counter_save_s, only if they changed since the
 * last save (flash wear); gw_lio_save_due() decides when.
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
    uint8_t local;            /* IO-7: outputs driven by a rule */
    uint8_t rule_count;
    struct {
        uint8_t di, dout, mode;
    } rules[GW_LOCAL_IO_CHANNELS];
    uint64_t seen[GW_LOCAL_IO_CHANNELS];   /* activation count of each DI at the last eval */
    uint8_t safe_on;          /* outputs whose safe state is ON */
    uint8_t safe_off;         /* outputs whose safe state is OFF (the others hold) */
    uint32_t failsafe_ms;     /* 0: an MQTT loss never trips the safe state */
    bool session;             /* last session state seen */
    uint32_t down_since_ms;
    bool tripped;
} gw_lio_t;

/** Outputs to keep across a restart (do_failsafe "hold") for gw_dout_init(). */
uint8_t gw_lio_hold_mask(const gw_config_t *cfg);

/** Start with the session down since @p now_ms (the boot). The safe state of an MQTT loss
 *  leaves out the outputs of rules (safe_on / safe_off); the restart state does not. */
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

/** IO-7: activations counted before @p count (restored at boot) do not toggle anything. */
void gw_lio_rules_start(gw_lio_t *l, const uint64_t *count);

/**
 * IO-7: outputs the rules change now, from the debounced DIs @p di, their activation
 * counters @p count and the current DO states @p current: the outputs of @p mask must take
 * the states in @p value (mask 0: nothing to do).
 */
void gw_lio_rules(gw_lio_t *l, uint8_t di, const uint64_t *count, uint8_t current, uint8_t *mask,
                  uint8_t *value);

/** IO-7: index of the rule that drives DO @p index, or -1 (NCMD allowed). */
int gw_lio_rule_of(const gw_lio_t *l, uint8_t index);

typedef struct {
    uint32_t period_ms;       /* 0: never saved */
    uint32_t last_ms;         /* last save (or the boot) */
    uint64_t saved[GW_LOCAL_IO_CHANNELS];
} gw_lio_save_t;

/** Counters @p restored (from NVS, or zeros) at @p now_ms. */
void gw_lio_save_init(gw_lio_save_t *s, const gw_config_t *cfg, const uint64_t *restored,
                      uint32_t now_ms);

/** True when @p count must be saved now: saving is enabled, it changed, and the period has
 *  run since the last save or @p force (orderly restart). The caller saves, then calls
 *  gw_lio_saved(). */
bool gw_lio_save_due(const gw_lio_save_t *s, const uint64_t *count, uint32_t now_ms, bool force);

/** @p count was saved at @p now_ms. */
void gw_lio_saved(gw_lio_save_t *s, const uint64_t *count, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
