/* Local I/O: 8 debounced digital inputs (IO-1) and 8 digital outputs on the
 * TCA9554 I2C expander (IO-2). Pins and active levels come from gw_bsp; the
 * states published through gw_tagdb are logical (1 = input asserted / output on). */
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
 * Switch every DO off: the latch is loaded with each channel's OFF level before the pins
 * become outputs, so there is no glitch (see gw_dout.h). Needs no configuration and runs
 * before it is loaded, so a rejected configuration never leaves an output energised from
 * before a warm reset (the TCA9554 keeps its latch across an ESP32 reset).
 * Returns the error if the expander does not answer: the DOs are then unknown (null in
 * gw_tagdb), DO commands are refused and gw_io_outputs_recover() retries.
 */
esp_err_t gw_io_outputs_off(void);

/**
 * If gw_io_outputs_off() could not reach the expander, retry it with a backoff (1 s doubling
 * to 60 s). Returns true when the outputs just became available (all off, state in gw_tagdb):
 * the caller republishes the DOs. Called periodically by the DO owner (Sparkplug task).
 */
bool gw_io_outputs_recover(void);

/**
 * Start sampling the DIs every CONFIG_GW_DI_SAMPLE_US with the configured debounce.
 * @p on_change is called from the esp_timer task whenever a debounced DI changes.
 */
esp_err_t gw_io_start_inputs(const gw_config_t *cfg, void (*on_change)(void));

/** Switch DO @p index (0-7) on or off and store the read-back state in gw_tagdb. Single caller.
 *  Returns false (and changes nothing) if gw_io_outputs_off() could not reach the expander. */
bool gw_io_set_do(uint8_t index, bool on);

#if CONFIG_GW_IO_SIMULATED
/** Emulator only: set the logical level of DI @p index (0-7); used by the console shell. */
void gw_io_sim_set_di(uint8_t index, bool active);
#endif

#ifdef __cplusplus
}
#endif
