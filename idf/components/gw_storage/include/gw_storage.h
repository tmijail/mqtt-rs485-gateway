/* Persistent storage: Sparkplug bdSeq in NVS (SPB-3) and the configuration
 * document (CFG-2): /cfg/config.json on the LittleFS "storage" partition if
 * present, otherwise the default embedded in the firmware. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialise NVS (erasing it if its layout is from another IDF version). */
esp_err_t gw_storage_init(void);

/** bdSeq for a new connection attempt; the following value is persisted first. */
uint8_t gw_storage_next_bdseq(void);

/**
 * Load and validate the configuration. @p source receives a description of
 * where it came from. Returns false (with @p err filled) if it is invalid.
 */
bool gw_storage_load_config(gw_config_t *cfg, gw_cfg_err_t *err, const char **source);

#ifdef __cplusplus
}
#endif
