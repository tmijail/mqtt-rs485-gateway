/* Local I/O: 8 debounced digital inputs (IO-1) and 8 digital outputs on the
 * TCA9554 I2C expander (IO-2). State is published through gw_tagdb. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Switch every DO off (outputs are loaded before the pins become outputs, so
 * there is no glitch), then start sampling the DIs every CONFIG_GW_DI_SAMPLE_US.
 * @p on_change is called from the esp_timer task whenever a debounced DI changes.
 * If the expander does not answer the DIs still run and the error is returned.
 */
esp_err_t gw_io_init(const gw_config_t *cfg, void (*on_change)(void));

/** Drive DO @p index (0-7) and store the read-back level in gw_tagdb. Single caller.
 *  Returns false (and changes nothing) if gw_io_init() could not reach the expander. */
bool gw_io_set_do(uint8_t index, bool on);

#ifdef __cplusplus
}
#endif
