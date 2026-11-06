/*
 * FW-1: firmware update policy, pure C (host-tested). The IDF side (components/gw_fw)
 * downloads and writes the image; this decides when a new image is healthy, when it is
 * rolled back, which URLs are acceptable and when progress is worth reporting.
 *
 * Health criterion of a new image ("trial", ESP_OTA_IMG_PENDING_VERIFY on the first boot):
 *   - confirmed as soon as its first complete Sparkplug session is up (NBIRTH and every DBIRTH
 *     out; issue #99: a session where a DBIRTH was too large for the broker does not count);
 *   - confirmed in commissioning mode (a factory reset left nothing to judge it by);
 *   - confirmed before a restart the operator asks for by saving a configuration or by a
 *     factory reset (issue #99: that restart would roll it back, and a configuration saved by
 *     the new image may not be accepted by the previous one);
 *   - rolled back at once when it rejects the stored configuration (recovery mode: the
 *     previous firmware accepted it) or cannot start at all (no memory for the configuration);
 *   - rolled back when no complete session came up within the timeout (whatever the cause, also
 *     a broker that is down: the previous firmware is the safe choice; the update can be
 *     repeated);
 *   - rolled back by the bootloader when it resets before any of the above (crash, watchdog,
 *     power cut, a plain restart).
 * Confirming is a write to otadata: when it fails the image stays on trial, without timeout
 * (it was judged healthy), and the write is retried with a backoff of 1 s to 60 s (issue #99).
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
    GW_OTA_TRIAL_CONFIRMING, /* judged healthy; marking it valid did not succeed yet */
    GW_OTA_TRIAL_CONFIRMED,  /* healthy and marked valid: the bootloader keeps it */
    GW_OTA_TRIAL_ROLLBACK,   /* rejected: back to the previous image */
} gw_ota_trial_state_t;

typedef enum {
    GW_OTA_KEEP = 0,         /* nothing to do now */
    GW_OTA_CONFIRM,          /* mark the running image valid, then gw_ota_trial_confirm_result() */
    GW_OTA_ROLLBACK,         /* mark it invalid and restart into the previous one */
} gw_ota_action_t;

typedef struct {
    gw_ota_trial_state_t state;
    uint32_t start_ms;       /* gw_port time of the boot (32-bit, wraps) */
    uint32_t timeout_ms;
    const char *why;         /* reason of the last decision (static text) */
    bool partial;            /* a session came up without every DBIRTH */
    bool in_flight;          /* CONFIRMING: a write to mark it valid is under way */
    uint32_t attempts;       /* failed writes to mark it valid */
    uint32_t retry_at_ms;    /* CONFIRMING: the next write */
} gw_ota_trial_t;

#define GW_OTA_NO_TICK UINT32_MAX   /* gw_ota_trial_next_ms(): nothing to wait for */
#define GW_OTA_RETRY_MIN_MS 1000u
#define GW_OTA_RETRY_MAX_MS 60000u

/** @p pending: the running image is on trial. */
void gw_ota_trial_init(gw_ota_trial_t *t, bool pending, uint32_t now_ms, uint32_t timeout_ms);
/** A Sparkplug session is up; @p births_missing: DBIRTHs that did not go out (too large for the
 *  broker). Only a complete session (0) confirms. */
gw_ota_action_t gw_ota_trial_session_up(gw_ota_trial_t *t, unsigned births_missing);
gw_ota_action_t gw_ota_trial_config_rejected(gw_ota_trial_t *t);
gw_ota_action_t gw_ota_trial_commissioning(gw_ota_trial_t *t);
/** The image cannot start at all (@p why: static text): rolled back at once. */
gw_ota_action_t gw_ota_trial_cannot_start(gw_ota_trial_t *t, const char *why);
/** The operator asks for a restart that keeps a change the new image made (a saved
 *  configuration, a factory reset; @p why: static text): CONFIRM while the trial is open (also
 *  to retry a failed write at once), KEEP otherwise. */
gw_ota_action_t gw_ota_trial_operator(gw_ota_trial_t *t, const char *why);
/** Issue #108 B1: a change that a restart puts into effect (a saved configuration, a factory
 *  reset) is refused once a rollback is decided (also when the rollback call failed and the image
 *  goes on running): the previous image would come back with what the new one stored. */
bool gw_ota_trial_commit_allowed(const gw_ota_trial_t *t);
/** Issue #108 B5: a plain restart asked by the operator (NCMD Reboot, shell, web) of an image
 *  judged healthy whose confirmation is still failing: CONFIRM (retry it now, so the restart does
 *  not roll back a healthy image); KEEP otherwise (a pending trial is rejected by the restart, the
 *  way to reject it by hand). */
gw_ota_action_t gw_ota_trial_restart_requested(gw_ota_trial_t *t);
/** Outcome of the write a CONFIRM asked for; on failure the next one is due after the backoff
 *  (@p rnd: its jitter). */
void gw_ota_trial_confirm_result(gw_ota_trial_t *t, bool ok, uint32_t now_ms, uint32_t rnd);
/** Call when gw_ota_trial_next_ms() says: the timeout of a pending trial (ROLLBACK), the retry
 *  of a failed confirmation (CONFIRM). */
gw_ota_action_t gw_ota_trial_tick(gw_ota_trial_t *t, uint32_t now_ms);
/** Milliseconds until gw_ota_trial_tick() has something to do (0: now), GW_OTA_NO_TICK if never. */
uint32_t gw_ota_trial_next_ms(const gw_ota_trial_t *t, uint32_t now_ms);
/** Milliseconds left before the timeout rolls the image back (0 if not pending). */
uint32_t gw_ota_trial_left_ms(const gw_ota_trial_t *t, uint32_t now_ms);
/** The trial is not decided yet: no other update is accepted meanwhile. */
bool gw_ota_trial_open(const gw_ota_trial_t *t);

/* Issue #101: what a refused update request does to the status (shell, web, Sparkplug). */
typedef enum {
    GW_OTA_REFUSAL_QUIET = 0,    /* a transfer is running: it keeps its own state */
    GW_OTA_REFUSAL_REPORT,       /* the refusal becomes the state ("refused: ...") */
    GW_OTA_REFUSAL_IN_TRIAL,     /* reported, but the trial phase stays (time left, outcome) */
} gw_ota_refusal_t;

gw_ota_refusal_t gw_ota_refusal(const gw_ota_trial_t *t, bool transfer_running);

/** An update URL: https:// (http:// only with @p allow_http), a host, printable ASCII without
 *  spaces, shorter than GW_OTA_URL_MAX. On false @p why says why. */
bool gw_ota_url_ok(const char *url, bool allow_http, char *why, size_t cap);

/** Body of POST /api/firmware with a URL: {"url": "https://..."} (nothing else). Copies the
 *  URL to @p out (GW_OTA_URL_MAX); on false @p why says why. Does not validate the URL. */
bool gw_ota_parse_request(const char *body, size_t len, char *out, char *why, size_t cap);

/* ---------- issue #102: what an image declares about itself ----------
 * A block right after the image's esp_app_desc_t (section .rodata_custom_desc, at
 * GW_OTA_CAPS_OFFSET from the start of the image: image header 24 B, first segment header 8 B,
 * esp_app_desc_t 256 B), little endian: magic "GWCP", version, size, flags. Newer versions may
 * grow it; the first 12 bytes keep their meaning. Images built before it have none. */
#define GW_OTA_CAPS_OFFSET 288u
#define GW_OTA_CAPS_SIZE 32u
#define GW_OTA_CAPS_MAGIC 0x50435747u   /* "GWCP" */
#define GW_OTA_CAPS_VERSION 1u
#define GW_OTA_CAP_NVS_ENCRYPTION 0x1u     /* built with CONFIG_NVS_ENCRYPTION (CFG-7) */
#define GW_OTA_CAP_FLASH_ENCRYPTION 0x2u   /* built with flash encryption (FW-3); informative */

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;           /* of the block */
    uint32_t flags;          /* GW_OTA_CAP_* */
    uint32_t reserved[5];
} gw_ota_caps_t;

/** The flags of the block at @p block (@p len bytes from GW_OTA_CAPS_OFFSET of an image); false
 *  when there is none (no magic, an unknown version 0, a size below 12 or above @p len). */
bool gw_ota_caps_read(const uint8_t *block, size_t len, uint32_t *flags);

/** Issue #102: may the image replace the running one? @p declared, @p flags: from
 *  gw_ota_caps_read(); @p nvs_encrypted: this device's NVS partition is encrypted (or may be: a
 *  migration pending). An image without NVS encryption cannot read it: it would erase it
 *  (bdSeq, secrets). On false @p why says why. */
bool gw_ota_caps_accept(bool declared, uint32_t flags, bool nvs_encrypted, char *why, size_t cap);

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
