/* Local I/O: 8 debounced digital inputs (IO-1) and 8 digital outputs on the
 * TCA9554 I2C expander (IO-2) with their safe state (IO-3). Pins and active levels come
 * from gw_bsp; the states published through gw_tagdb are logical (1 = input asserted /
 * output on).
 *
 * Single owner of the expander: before the configuration is loaded, app_main sets the
 * outputs up (gw_io_outputs_boot, gw_io_outputs_all_off); from gw_io_start() on, only the
 * local_io task writes them: DO commands (gw_io_set_do, from the Sparkplug task), the safe
 * state and the retries of an expander that does not answer. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"
#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * First step of a restart, before the configuration is loaded: every DO off, except the
 * outputs of @p hold (IO-3 "hold" of the last valid configuration, kept in NVS) whose pin
 * the expander still drives from before a warm reset: they keep their state. The latch is
 * loaded before the pins become outputs, so there is no glitch (see gw_dout.h).
 * Returns the error if the expander does not answer: the DOs are then unknown (null in
 * gw_tagdb), DO commands are refused and the local_io task retries with a backoff.
 */
esp_err_t gw_io_outputs_boot(uint8_t hold);

/** No valid configuration (recovery, commissioning): held outputs are switched off too. */
void gw_io_outputs_all_off(void);

/**
 * With a valid configuration: put the DOs in their restart state (do_failsafe "on" / "off",
 * "hold" kept), start the local_io task and the DI sampling every CONFIG_GW_DI_SAMPLE_US
 * with the configured debounce. @p on_change is called whenever a debounced DI or a DO
 * changed (from the esp_timer task or the local_io task).
 */
esp_err_t gw_io_start(const gw_config_t *cfg, void (*on_change)(void));

/** Switch DO @p index (0-7) on or off through the local_io task and wait for it (at most
 *  500 ms). The read-back state is in gw_tagdb. Single caller (the Sparkplug task).
 *  Returns false (and changes nothing) if the expander is not available. */
bool gw_io_set_do(uint8_t index, bool on);

#if CONFIG_GW_IO_SIMULATED
/** Emulator only: set the logical level of DI @p index (0-7); used by the console shell. */
void gw_io_sim_set_di(uint8_t index, bool active);
#endif

#ifdef __cplusplus
}
#endif
