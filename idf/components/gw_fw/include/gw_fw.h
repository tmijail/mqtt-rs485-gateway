/*
 * FW-1: firmware updates over HTTPS (esp_https_ota) or uploaded through the web UI, into the
 * other OTA slot, with automatic rollback (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE). Issue #144
 * gives the process (README 3.6):
 *
 *   - gw_fw_start_url(): NCMD "Node Control/Firmware Update" or POST /api/firmware {"url"}.
 *     Downloads in its own task (created for the update, 8 KB, priority 2, core 0); the
 *     Sparkplug task never blocks on it. TLS: ota.ca_file, else mqtt.tls.ca_file, else the
 *     ESP-IDF bundle of public root CAs.
 *   - gw_fw_upload_*(): POST /api/firmware with the image as the body (web server task).
 *   - gw_fw_boot_other(): the "Restart into <version>" button (POST /api/firmware/other) and NCMD
 *     "Node Control/Boot Other Slot": the image in the other slot boots on trial.
 *   - Before any write, the pre-flight check (gw_ota_preflight) refuses an image that cannot read
 *     the stored configuration, NVS or LittleFS; the web UI and the API can force it (the image
 *     must then reach recovery mode or better). The expected mode is stored with the update
 *     (fw_last) before esp_ota_set_boot_partition().
 *   - A new image is on trial: the self-check (gw_ota policy) confirms it at 60 s, or rejects it
 *     (a planned restart: the bootloader returns to the earlier image). Any reset during the
 *     trial also returns to the earlier image. During the trial, configuration saves, updates and
 *     the button are refused ("testing <v>: N s left").
 *   - The boot-loop counter (RTC memory, gw_bootloop) starts safe mode after 3 crashes in a row.
 * Progress and results: gw_fw_get_status(), hooks.changed on every change.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "gw_config.h"
#include "gw_fwlast.h"
#include "gw_ota.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GW_FW_IDLE = 0,
    GW_FW_DOWNLOADING,      /* or receiving an upload */
    GW_FW_RESTARTING,       /* written and verified (or rejected): restarting */
    GW_FW_FAILED,           /* the last update failed (text says why) */
    GW_FW_TESTING,          /* running a new image that is not confirmed yet */
    GW_FW_INSTALLED,        /* the new image is confirmed */
    GW_FW_ROLLED_BACK,      /* running the earlier image: the new one was rejected */
} gw_fw_phase_t;

/* The image in the other slot (issue #144 item 6). */
typedef enum {
    GW_FW_SLOT_EMPTY = 0,   /* no application image */
    GW_FW_SLOT_PREVIOUS,    /* an image that ran confirmed, or one that was flashed */
    GW_FW_SLOT_NEW,         /* set to boot or on trial, not confirmed yet */
    GW_FW_SLOT_REJECTED,    /* rejected in its trial (otadata: aborted or invalid) */
    GW_FW_SLOT_INVALID,     /* its verification failed in this boot */
} gw_fw_slot_t;

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
    char other_slot[17];    /* its partition label */
    gw_fw_slot_t other_state;
    char other_why[GW_FWLAST_MAX];   /* REJECTED: the cause that fw_last keeps ("" if none) */
    gw_ota_mode_t expected; /* a new image on trial: the mode that it must reach */
    uint32_t trial_left_s;  /* GW_FW_TESTING: seconds before the self-check can confirm it */
    bool trial;             /* the trial is open: saves, updates and the button are refused */
    uint32_t boot_count;    /* the boot-loop counter (crash resets in a row) */
    bool safe_mode;         /* this boot is in safe mode (repeated crashes) */
    bool enabled;           /* updates accepted (ota.enabled) */
    unsigned gen;           /* incremented on every change */
} gw_fw_status_t;

typedef struct {
    void (*changed)(void);  /* status changed (any task; must not block) */
    void (*restart)(void);  /* planned restart (NDEATH first in normal mode) */
    /** The self-check items now (issue #144 item 1): the network driver, the web server loop. */
    void (*self_check)(gw_ota_selfcheck_t *out);
} gw_fw_hooks_t;

/** Issue #144 item 4: the first call of the boot, before gw_storage_init(). Updates the boot-loop
 *  counter from the reset reason; true when this boot must start safe mode (3 crashes in a row,
 *  and the running image is not on trial). */
bool gw_fw_bootloop_start(void);

/** After gw_storage_init(), before the console shell starts (issue #101): the status lock, the
 *  slots, the record of the last update (fw_last) and, for a new image on trial, its expected
 *  mode and the start of its trial. Single task; gw_fw_init() calls it if it did not run. */
void gw_fw_early_init(void);

/** At boot, before the mode is reported. @p cfg: the running configuration (recovery, safe mode
 *  and commissioning: the network-only one); @p enabled: updates accepted. */
esp_err_t gw_fw_init(const gw_config_t *cfg, bool enabled, const gw_fw_hooks_t *hooks);

/** Issue #144 item 2: the gateway reached @p mode (@p detail: the JSON path or the reason of a
 *  rejected configuration). A new image on trial below its expected mode is rejected at once
 *  (planned restart). Normal mode also clears the boot-loop counter after 120 s. */
void gw_fw_mode(gw_ota_mode_t mode, const char *detail);

/** Issue #144 item 4: this boot is in safe mode; normal mode is tried again after 60 min. */
void gw_fw_safe_mode(void);

/** Issue #209: true when this boot is in safe mode; @p requested: an operator asked for it (else:
 *  repeated crashes); @p left_s: the seconds until the restart into normal mode. */
bool gw_fw_safe_mode_info(bool *requested, uint32_t *left_s);

/** Issue #209: the next boot starts safe mode (POST /api/restart?safe=1, the shell's 'restart
 *  safe'); the caller then restarts the gateway. False (and the reason in @p why) during an
 *  update, during the trial of a new image, and in safe mode. */
bool gw_fw_request_safe_mode(char *why, size_t cap);

/** The boot cannot go on (@p cause: "no memory to start", ...). A new image on trial is rejected
 *  at once (fw_last, then a restart: does not return then); otherwise nothing. Works after
 *  gw_fw_early_init() alone. */
void gw_fw_cannot_start(const char *cause);

/** The self-check of a new image on trial (main task, about once a second): confirms it, retries
 *  a failed confirmation, or rejects it at the backstop. Nothing when no trial is open. */
void gw_fw_trial_poll(void);

/** gw_fw_trial_poll() until the trial is over (returns at once when there is none). */
void gw_fw_trial_wait(void);

/** Before every planned restart: an image that passed the self-check but whose confirmation keeps
 *  failing gets one more synchronous try (issue #99); a restart that the operator asked for clears
 *  the boot-loop counter. The caller restarts anyway. */
void gw_fw_restart_requested(void);

/** Start a download; ESP_OK if it started (the result comes later in the status). @p force:
 *  install also when the pre-flight check refuses (not for NCMD). ESP_ERR_NOT_SUPPORTED: the
 *  pre-flight check refused before the download (cannot happen: the check needs the image head;
 *  see the status). */
esp_err_t gw_fw_start_url(const char *url, bool force, char *why, size_t cap);

/** Upload in pieces (one at a time, from one task). @p size: the image size if known (0).
 *  gw_fw_upload_write() returns ESP_ERR_NOT_SUPPORTED when the pre-flight check refuses the
 *  image (HTTP 409; @p why gives the reasons; force with @p force at begin). */
esp_err_t gw_fw_upload_begin(size_t size, bool force, char *why, size_t cap);
esp_err_t gw_fw_upload_write(const void *data, size_t len, char *why, size_t cap);
/** @p complete false: abandon. On ESP_OK the gateway restarts into the new image. */
esp_err_t gw_fw_upload_end(bool complete, char *why, size_t cap);

/** Issue #144 item 6: boot the image of the other slot on trial (planned restart). Refused
 *  (ESP_ERR_INVALID_STATE) during a trial or a transfer, (ESP_ERR_NOT_FOUND) when the slot is
 *  empty or invalid, (ESP_ERR_NOT_SUPPORTED) when the pre-flight check refuses and not @p force. */
esp_err_t gw_fw_boot_other(bool force, char *why, size_t cap);

void gw_fw_get_status(gw_fw_status_t *out);

/** A new image is on trial (TESTING, CONFIRMING or REJECTED). */
bool gw_fw_on_trial(void);

/** The boot-loop counter (crash resets in a row). */
uint32_t gw_fw_boot_count(void);

/** Only the state text and progress (what the Sparkplug metrics carry). */
void gw_fw_get_state(char *text, size_t cap, uint8_t *progress);

#ifdef __cplusplus
}
#endif
