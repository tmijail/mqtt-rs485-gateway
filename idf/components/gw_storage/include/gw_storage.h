/* Persistent storage: Sparkplug bdSeq in NVS (SPB-3) and the configuration
 * document (CFG-1, CFG-2): /cfg/config.json on the LittleFS "storage"
 * partition. The default embedded in the firmware is used only when that file
 * does not exist; an unmountable partition, an unreadable, empty or oversized
 * file, or an invalid document are errors.
 *
 * The secrets of the document (mqtt.password, network.wifi.password and the hash of
 * web.password) live in NVS, not in the file (gw_cfgdoc.h). New documents (CFG-4, CFG-6)
 * are validated and written atomically (temporary file + rename). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "gw_cfgdoc.h"
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
 * where it came from. Returns false, with the reason in @p err, if the file
 * cannot be used or the document is invalid: the caller must then not start.
 */
bool gw_storage_load_config(gw_config_t *cfg, gw_cfg_err_t *err, const char **source);

/**
 * Validate a new configuration document (gw_cfgdoc update rules for the secrets) and,
 * unless @p dry_run, store it: secrets to NVS, the document without them to
 * /cfg/config.json, atomically. It takes effect at the next restart.
 * Returns ESP_OK; ESP_ERR_INVALID_ARG (invalid document: @p err has the message and the
 * JSON path or line and column), ESP_ERR_INVALID_SIZE (over CONFIG_GW_CONFIG_MAX_BYTES),
 * ESP_ERR_NO_MEM, or another error if storing failed (@p err says what).
 * Safe from any task; one save at a time.
 */
esp_err_t gw_storage_save_config(const char *json, size_t len, bool dry_run, gw_cfg_err_t *err);

/**
 * The stored document (or the embedded default if there is no file) for export, with every
 * secret masked. NULL with the reason in @p err. Release with gw_storage_free().
 */
char *gw_storage_export_config(size_t *len, gw_cfg_err_t *err);

#define GW_COMMISSION_PW_LEN 12

/**
 * The device's commissioning password: random, created at the first boot (and after a
 * factory reset), kept in NVS. It opens the web UI for user "admin" while no web password
 * is configured and protects the commissioning access point (NET-5).
 */
void gw_storage_commissioning_password(char out[GW_COMMISSION_PW_LEN + 1]);

/** Secrets currently stored (empty strings when none). */
void gw_storage_get_secrets(gw_secrets_t *out);

void gw_storage_free(void *ptr);

#ifdef __cplusplus
}
#endif
