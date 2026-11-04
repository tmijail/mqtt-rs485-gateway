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
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"
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

/** Issue #94: true in an encrypted build whose NVS is still unencrypted this boot (a new
 *  firmware on trial keeps it as the previous image left it, or there was no place for the
 *  reset-safe copy). The next boot of a confirmed image encrypts it. */
bool gw_storage_nvs_encryption_pending(void);

/** CFG-7: true when NVS (and with it the secrets) is encrypted (CONFIG_NVS_ENCRYPTION,
 *  HMAC key in eFuse). Development builds without it keep the secrets in plain NVS. */
bool gw_storage_secrets_encrypted(void);

/** Issue #64: the web server's certificate and private key (PEM), kept with the secrets in
 *  NVS (encrypted in production). False if there is none yet. */
bool gw_storage_get_web_tls(char *cert, size_t cert_cap, char *key, size_t key_cap);

esp_err_t gw_storage_set_web_tls(const char *cert, const char *key);

/** Secrets currently stored (empty strings when none). */
void gw_storage_get_secrets(gw_secrets_t *out);

void gw_storage_free(void *ptr);

/**
 * CFG-5: format the LittleFS partition and erase NVS (configuration, secrets, bdSeq, the
 * commissioning password), then mark the device for commissioning. The caller restarts.
 */
esp_err_t gw_storage_factory_reset(void);

/**
 * True after a factory reset until a configuration is saved (or flashed): the gateway then
 * boots into commissioning mode instead of the embedded example configuration.
 */
bool gw_storage_commissioning_pending(void);

/** Remember the network (and web port) of a configuration that was accepted, for recovery. */
void gw_storage_save_last_good(const gw_config_t *cfg);

/**
 * Configuration to run the network and the web server with when the stored one was
 * rejected or is absent (CFG-3 recovery, CFG-5 commissioning): the network and web port of
 * the last valid configuration, or DHCP on Ethernet and port 80. Nothing else is set.
 */
void gw_storage_recovery_config(gw_config_t *cfg);

/** Largest file next to config.json that gw_storage_read_file() reads. */
#define GW_STORAGE_FILE_MAX 16384

/**
 * Whole file /cfg/<@p name> (a TLS certificate or key, MQTT-4/5), NUL terminated, in PSRAM
 * when present. NULL with the reason (Spanish, with the path) in @p why. Release with
 * gw_storage_free(). The partition must have been mounted by gw_storage_load_config().
 */
char *gw_storage_read_file(const char *name, size_t *len, char *why, size_t why_cap);

/**
 * Small state of the local I/O kept in NVS (IO-3 outputs to hold across a restart, IO-4 pulse
 * counters, ...): blob @p key of @p len bytes. Written only when it changed (flash wear).
 * Refused (ESP_ERR_INVALID_STATE) after gw_storage_factory_reset(), so nothing of the old
 * device survives the reset through a late write (for example at the restart).
 */
esp_err_t gw_storage_set_blob(const char *key, const void *data, size_t len);

/** Read blob @p key of exactly @p len bytes into @p out; false (out untouched) if absent. */
bool gw_storage_get_blob(const char *key, void *out, size_t len);

/**
 * Zeroed allocation for a large structure that lives for the whole run (configuration,
 * Sparkplug engine): PSRAM when the board has it, internal RAM otherwise. Logs where it
 * landed under @p what. NULL if neither has room. Boot time only (REL-2).
 */
void *gw_storage_big_alloc(size_t size, const char *what);

/** Issue #80: the smallest "largest free internal block" seen at the peak of a configuration
 *  validation (gw_storage_save_config), 0 if none ran yet. */
size_t gw_storage_validation_min_block(void);

#if CONFIG_GW_IO_SIMULATED
/** Emulator only (issue #84): the next configuration validation keeps its buffers for @p ms at
 *  its peak, so the E2E can make a TLS handshake happen during it. */
void gw_storage_sim_hold_validation(uint32_t ms);
#endif

#ifdef __cplusplus
}
#endif
