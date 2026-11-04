/*
 * FW-1: firmware update policy, pure C (host-tested). The IDF side (components/gw_ota)
 * downloads and writes the image; this decides when a new image is healthy, when it is
 * rolled back, which URLs are acceptable and when progress is worth reporting.
 *
 * Health criterion of a new image ("trial", ESP_OTA_IMG_PENDING_VERIFY on the first boot):
 *   - confirmed as soon as its first Sparkplug session is up (NBIRTH and every DBIRTH out);
 *   - confirmed in commissioning mode (a factory reset left nothing to judge it by);
 *   - rolled back at once when it rejects the stored configuration (recovery mode: the
 *     previous firmware accepted it);
 *   - rolled back when no session came up within the timeout (whatever the cause, also a
 *     broker that is down: the previous firmware is the safe choice; the update can be
 *     repeated);
 *   - rolled back by the bootloader when it resets before any of the above (crash,
 *     watchdog, power cut).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_OTA_URL_MAX 256   /* including the terminator */

typedef enum {
    GW_OTA_TRIAL_NONE = 0,   /* the running image is not on trial (confirmed or flashed) */
    GW_OTA_TRIAL_PENDING,    /* first boot of a new image: waiting for its first session */
    GW_OTA_TRIAL_CONFIRMED,  /* healthy: the bootloader keeps it */
    GW_OTA_TRIAL_ROLLBACK,   /* rejected: back to the previous image */
} gw_ota_trial_state_t;

typedef enum {
    GW_OTA_KEEP = 0,         /* nothing to do now */
    GW_OTA_CONFIRM,          /* mark the running image valid */
    GW_OTA_ROLLBACK,         /* mark it invalid and restart into the previous one */
} gw_ota_action_t;

typedef struct {
    gw_ota_trial_state_t state;
    uint32_t start_ms;       /* gw_port time of the boot (32-bit, wraps) */
    uint32_t timeout_ms;
    const char *why;         /* reason of the last decision (static text) */
} gw_ota_trial_t;

/** @p pending: the running image is on trial. */
void gw_ota_trial_init(gw_ota_trial_t *t, bool pending, uint32_t now_ms, uint32_t timeout_ms);
gw_ota_action_t gw_ota_trial_session_up(gw_ota_trial_t *t);
gw_ota_action_t gw_ota_trial_config_rejected(gw_ota_trial_t *t);
gw_ota_action_t gw_ota_trial_commissioning(gw_ota_trial_t *t);
/** Call periodically (and when the timeout expires). */
gw_ota_action_t gw_ota_trial_tick(gw_ota_trial_t *t, uint32_t now_ms);
/** Milliseconds left before the timeout rolls the image back (0 if not pending). */
uint32_t gw_ota_trial_left_ms(const gw_ota_trial_t *t, uint32_t now_ms);

/** An update URL: https:// (http:// only with @p allow_http), a host, printable ASCII without
 *  spaces, shorter than GW_OTA_URL_MAX. On false @p why says why. */
bool gw_ota_url_ok(const char *url, bool allow_http, char *why, size_t cap);

/** Body of POST /api/firmware with a URL: {"url": "https://..."} (nothing else). Copies the
 *  URL to @p out (GW_OTA_URL_MAX); on false @p why says why. Does not validate the URL. */
bool gw_ota_parse_request(const char *body, size_t len, char *out, char *why, size_t cap);

/* Progress of a download, reported every 10 % (or every 5 s when the size is unknown). */
typedef struct {
    uint32_t total;          /* image size, 0 = unknown */
    uint32_t done;
    uint8_t reported_pct;
    uint32_t reported_ms;
    bool any;
} gw_ota_progress_t;

void gw_ota_progress_init(gw_ota_progress_t *p, uint32_t total, uint32_t now_ms);
/** Record @p done bytes; true when the new percentage (*pct) should be published. */
bool gw_ota_progress_update(gw_ota_progress_t *p, uint32_t done, uint32_t now_ms, uint8_t *pct);

#ifdef __cplusplus
}
#endif
