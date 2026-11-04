/*
 * FW-1: firmware updates over HTTPS (esp_https_ota) or uploaded through the web UI, into the
 * other OTA slot, with automatic rollback (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE).
 *
 *   - gw_fw_start_url(): NCMD "Node Control/Firmware Update" or POST /api/firmware {"url"}.
 *     Downloads in its own task (created for the update, 8 KB, priority 2, core 0); the
 *     Sparkplug task never blocks on it. TLS: ota.ca_file, else mqtt.tls.ca_file, else the
 *     ESP-IDF bundle of public root CAs.
 *   - gw_fw_upload_*(): POST /api/firmware with the image as the body (web server task).
 *   - Either way the image must be an app of this project; esp_ota_end() checks it (and its
 *     signature when signed images are required, FW-2). On success the gateway restarts
 *     through hooks.restart (orderly: NDEATH first).
 *   - A new image is on trial until gw_fw_session_up() (its first complete Sparkplug session);
 *     the policy (gw_core gw_ota.h) rolls it back on timeout or when it rejects the
 *     configuration. A reset during the trial makes the bootloader roll it back, except the
 *     restart after a saved configuration or a factory reset: gw_storage asks gw_fw to confirm
 *     the image first (issue #99).
 * Progress and results: gw_fw_get_status(), hooks.changed on every change.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GW_FW_IDLE = 0,
    GW_FW_DOWNLOADING,      /* or receiving an upload */
    GW_FW_REBOOTING,        /* written and verified: restarting into it */
    GW_FW_FAILED,           /* the last update failed (text says why) */
    GW_FW_TRIAL,            /* running a new image that is not confirmed yet */
    GW_FW_CONFIRMED,        /* the new image confirmed itself in this boot */
    GW_FW_ROLLED_BACK,      /* running the previous image: the new one was rejected */
} gw_fw_phase_t;

typedef struct {
    gw_fw_phase_t phase;
    uint8_t progress;       /* % of the current (or last) transfer */
    uint32_t bytes;
    uint32_t total;         /* 0 = unknown */
    char text[96];          /* "Firmware Update/State" (GW_NODE_FW_STATE_MAX, issue #87) */
    char detail[240];       /* longer reason (log, shell, web) */
    char running[32];       /* running version */
    char slot[17];          /* its partition label */
    char other[32];         /* version in the other slot ("" if none) */
    uint32_t trial_left_s;  /* GW_FW_TRIAL: seconds before the rollback */
    bool enabled;           /* updates accepted (ota.enabled) */
    unsigned gen;           /* incremented on every change */
} gw_fw_status_t;

typedef struct {
    void (*changed)(void);  /* status changed (any task; must not block) */
    void (*restart)(void);  /* orderly restart into the new image */
} gw_fw_hooks_t;

/** First thing at boot, before the console shell starts (issue #101): the status lock, the
 *  running and other slots, and whether the running image is on trial (its trial starts here).
 *  Single task; gw_fw_init() calls it if it did not run. */
void gw_fw_early_init(void);

/** At boot, before anything judges the image. @p cfg: the running configuration (recovery
 *  and commissioning: the network-only one); @p enabled: updates accepted. */
esp_err_t gw_fw_init(const gw_config_t *cfg, bool enabled, const gw_fw_hooks_t *hooks);

/** Health signals for a new image on trial (gw_ota policy). @p births_missing: DBIRTHs of the
 *  session that did not go out (too large for the broker, issue #99): only a complete session
 *  confirms the image. */
void gw_fw_session_up(unsigned births_missing);
void gw_fw_config_rejected(void);
void gw_fw_commissioning(void);
/** Issue #99: the boot cannot go on (@p why, static text: no memory for the configuration). A
 *  new image on trial is rolled back at once (does not return then); otherwise nothing. Works
 *  after gw_fw_early_init() alone. */
void gw_fw_cannot_start(const char *why);

/** Start a download; ESP_OK if it started (the result comes later in the status). */
esp_err_t gw_fw_start_url(const char *url, char *why, size_t cap);

/** Upload in pieces (one at a time, from one task). @p size: the image size if known (0). */
esp_err_t gw_fw_upload_begin(size_t size, char *why, size_t cap);
esp_err_t gw_fw_upload_write(const void *data, size_t len, char *why, size_t cap);
/** @p complete false: abandon. On ESP_OK the gateway restarts into the new image. */
esp_err_t gw_fw_upload_end(bool complete, char *why, size_t cap);

void gw_fw_get_status(gw_fw_status_t *out);

/** Only the state text and progress (what the Sparkplug metrics carry). */
void gw_fw_get_state(char *text, size_t cap, uint8_t *progress);

#ifdef __cplusplus
}
#endif
