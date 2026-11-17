/*
 * FW-1: firmware update policy, pure C (host-tested). The IDF side (components/gw_fw)
 * downloads and writes the image; this decides when a new image is confirmed or rejected, what
 * an image may replace (the pre-flight check), which URLs are acceptable and when progress is
 * worth reporting. Issue #144 gives the design (README 3.6).
 *
 * The trial of a new image (ESP_OTA_IMG_PENDING_VERIFY on its first boot) is a self-check of the
 * gateway. It does not depend on external services (broker, Modbus slaves, NTP, update server):
 *   - confirmed at the first instant when the uptime is at least the self-check time (60 s), the
 *     gateway is in its expected mode, the network driver started and the web server loop is
 *     alive (when a web server runs);
 *   - rejected at once when the gateway cannot reach its expected mode (a configuration that it
 *     rejects after an update from normal mode) or cannot start (no memory);
 *   - rejected when the self-check is not true at the backstop (5 min);
 *   - rolled back by the bootloader when it resets before the confirmation (crash, watchdog,
 *     power cut). A rejection is a planned restart of the pending image: the bootloader then
 *     boots the earlier image too.
 * Confirming is a write to otadata: when it fails the image stays on trial (it passed the
 * self-check), and the write is retried with a backoff of 1 s to 60 s, and once before a planned
 * restart (issue #99).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gw_cfgschema.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_OTA_URL_MAX 256   /* including the terminator */
#define GW_OTA_CAUSE_MAX 80  /* a rejection cause, with the NUL */

/* The modes of the gateway, worst first. Safe mode is recovery mode with the cause "repeated
 * crashes" (issue #144 item 4). */
typedef enum {
    GW_OTA_MODE_NONE = 0,       /* not reached yet */
    GW_OTA_MODE_RECOVERY,       /* configuration rejected, safe mode */
    GW_OTA_MODE_COMMISSIONING,  /* after a factory reset (CFG-5) */
    GW_OTA_MODE_NORMAL,
} gw_ota_mode_t;

/** Issue #144 item 2: the mode that an image installed from @p from must reach. From normal mode
 *  without @p force: normal mode. From recovery (safe mode too) or commissioning, or with force:
 *  recovery mode or better. */
gw_ota_mode_t gw_ota_expected_mode(gw_ota_mode_t from, bool force);

/** True when @p actual satisfies @p expected (normal: only normal; recovery: any mode). */
bool gw_ota_mode_reached(gw_ota_mode_t expected, gw_ota_mode_t actual);

/** "normal", "recovery", "commissioning", "none". */
const char *gw_ota_mode_name(gw_ota_mode_t m);

typedef enum {
    GW_OTA_TRIAL_NONE = 0,   /* the running image is not on trial (confirmed or flashed) */
    GW_OTA_TRIAL_TESTING,    /* first boot of a new image: the self-check is not true yet */
    GW_OTA_TRIAL_CONFIRMING, /* the self-check passed; marking it valid did not succeed yet */
    GW_OTA_TRIAL_CONFIRMED,  /* marked valid: the bootloader keeps it */
    GW_OTA_TRIAL_REJECTED,   /* rejected: a planned restart returns to the earlier image */
} gw_ota_trial_state_t;

typedef enum {
    GW_OTA_KEEP = 0,         /* nothing to do now */
    GW_OTA_CONFIRM,          /* mark the running image valid, then gw_ota_trial_confirm_result() */
    GW_OTA_REJECT,           /* write the cause (fw_last), then restart: the bootloader returns */
} gw_ota_action_t;

/* The items of the self-check, as the IDF side sees them now. */
typedef struct {
    bool net_started;        /* the network driver started (not the link, not the broker) */
    bool web_needed;         /* a web server runs (normal mode with web.enabled, recovery) */
    bool web_alive;          /* its loop answered a ping in the last 5 s */
} gw_ota_selfcheck_t;

typedef struct {
    gw_ota_trial_state_t state;
    gw_ota_mode_t expected;
    gw_ota_mode_t mode;      /* the mode that the gateway reached (NONE: not yet) */
    uint32_t selfcheck_ms;   /* least uptime for the confirmation */
    uint32_t backstop_ms;    /* uptime of the rejection when the self-check is not true */
    char cause[GW_OTA_CAUSE_MAX];   /* REJECTED: why; CONFIRMED: "" */
    bool in_flight;          /* CONFIRMING: a write to mark it valid is under way */
    uint32_t attempts;       /* failed writes to mark it valid */
    uint32_t retry_at_ms;    /* CONFIRMING: the next write (uptime) */
} gw_ota_trial_t;

#define GW_OTA_RETRY_MIN_MS 1000u
#define GW_OTA_RETRY_MAX_MS 60000u
#define GW_OTA_WEB_PING_MS 5000u   /* web_alive: a ping of the web server loop at most this old */

/** @p pending: the running image is on trial. Times are uptimes in ms (the trial starts at the
 *  boot). */
void gw_ota_trial_init(gw_ota_trial_t *t, bool pending, gw_ota_mode_t expected,
                       uint32_t selfcheck_ms, uint32_t backstop_ms);

/** The gateway reached @p mode. A mode below the expected one rejects the image at once:
 *  "configuration rejected: <@p detail>" (recovery), "configuration rejected: commissioning". */
gw_ota_action_t gw_ota_trial_mode(gw_ota_trial_t *t, gw_ota_mode_t mode, const char *detail);

/** The image cannot start at all (@p cause: "no memory to start", ...): rejected at once. */
gw_ota_action_t gw_ota_trial_cannot_start(gw_ota_trial_t *t, const char *cause);

/** Call about once a second with the uptime and the self-check: CONFIRM once every item is true
 *  at or after the self-check time; REJECT ("self-check failed: <item>") at the backstop; the
 *  retries of a failed confirmation (CONFIRM). */
gw_ota_action_t gw_ota_trial_check(gw_ota_trial_t *t, uint32_t uptime_ms,
                                   const gw_ota_selfcheck_t *sc);

/** The backstop of a blocked main task (an esp_timer, after the backstop): REJECT with
 *  "self-check failed: <@p item>" while the image is still TESTING. */
gw_ota_action_t gw_ota_trial_backstop(gw_ota_trial_t *t, const char *item);

/** Outcome of the write a CONFIRM asked for; on failure the next one is due after the backoff
 *  (@p rnd: its jitter). */
void gw_ota_trial_confirm_result(gw_ota_trial_t *t, bool ok, uint32_t uptime_ms, uint32_t rnd);

/** Before a planned restart: CONFIRM when the self-check passed but the write still fails (one
 *  more try, issue #99); KEEP otherwise. */
gw_ota_action_t gw_ota_trial_before_restart(gw_ota_trial_t *t);

/** The trial is not over: configuration saves, updates and the other-slot button are refused
 *  (TESTING, CONFIRMING, REJECTED). */
bool gw_ota_trial_open(const gw_ota_trial_t *t);

/** Milliseconds until the self-check can confirm the image (or, after the self-check time, until
 *  the backstop); 0 when not TESTING. */
uint32_t gw_ota_trial_left_ms(const gw_ota_trial_t *t, uint32_t uptime_ms);

/** The refusal of a configuration save, an update or the button during the trial, for the HTTP
 *  409 answer: "testing <v>: N s left", "testing <v>: not marked valid yet", "<v> was rejected
 *  (<cause>): restarting into the earlier firmware". */
void gw_ota_trial_refusal(const gw_ota_trial_t *t, const char *version, uint32_t uptime_ms,
                          char *out, size_t cap);

/** An update URL: https:// (http:// only with @p allow_http), a host, printable ASCII without
 *  spaces, shorter than GW_OTA_URL_MAX. On false @p why says why. */
bool gw_ota_url_ok(const char *url, bool allow_http, char *why, size_t cap);

/* ---------- issue #215: the SHA-256 that the user gives for an image ---------- */
#define GW_OTA_SHA256_LEN 32     /* bytes */
#define GW_OTA_SHA256_HEX 65     /* 64 hexadecimal characters and the NUL */

/** @p hex: 64 hexadecimal characters (capital or small letters) into @p out. Anything else
 *  (other characters, spaces, another length, NULL) is false, and @p why (if not NULL) says why:
 *  "expected 64 hexadecimal characters (0-9, a-f), got 63". */
bool gw_ota_sha256_parse(const char *hex, uint8_t out[GW_OTA_SHA256_LEN], char *why, size_t cap);

/** The 64 small hexadecimal characters of @p sha with the NUL (GW_OTA_SHA256_HEX). */
void gw_ota_sha256_hex(const uint8_t sha[GW_OTA_SHA256_LEN], char out[GW_OTA_SHA256_HEX]);

/** Body of POST /api/firmware with a URL: {"url": "https://..."} and, as options, "force": true
 *  (issue #144 item 3) and "sha256": "<64 hexadecimal characters>" (issue #215; "" = no check).
 *  Copies the URL to @p out (GW_OTA_URL_MAX); @p has_sha tells whether @p sha holds a SHA-256.
 *  On false @p why says why. Does not validate the URL. */
bool gw_ota_parse_request(const char *body, size_t len, char *out, bool *force,
                          uint8_t sha[GW_OTA_SHA256_LEN], bool *has_sha, char *why, size_t cap);

/* ---------- issues #102 and #144: what an image declares about itself ----------
 * A block right after the image's esp_app_desc_t (section .rodata_custom_desc, at
 * GW_OTA_CAPS_OFFSET from the start of the image: image header 24 B, first segment header 8 B,
 * esp_app_desc_t 256 B), little endian. Version 1 (#102): magic "GWCP", version, size, flags.
 * Version 2 (#144 item 3, #148): also the highest configuration schema that the image reads, its
 * NVS format and its LittleFS disk version. Newer versions may grow it; the fields keep their
 * offsets. Images built before #102 have none. */
#define GW_OTA_CAPS_OFFSET 288u
#define GW_OTA_CAPS_SIZE 32u
#define GW_OTA_CAPS_MAGIC 0x50435747u   /* "GWCP" */
#define GW_OTA_CAPS_VERSION 2u
#define GW_OTA_CAP_NVS_ENCRYPTION 0x1u     /* built with CONFIG_NVS_ENCRYPTION (CFG-7) */
#define GW_OTA_CAP_FLASH_ENCRYPTION 0x2u   /* built with flash encryption (FW-3); informative */

/* nvs_format: the IDF NVS page format in the high byte (0xFF minus the page version that IDF
 * writes: IDF counts it down), the format of the gateway's own NVS keys and records in the low
 * byte (GW_NVS_RECORDS). An image with a lower byte than the running one cannot read the NVS. */
#define GW_NVS_RECORDS 1u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;           /* of the block */
    uint32_t flags;          /* GW_OTA_CAP_* */
    uint16_t cfg_schema;     /* v2: GW_CONFIG_SCHEMA */
    uint16_t nvs_format;     /* v2 */
    uint32_t lfs_disk;       /* v2: LFS_DISK_VERSION (major << 16 | minor) */
    uint32_t reserved[3];
} gw_ota_caps_t;

/* What an image declares, as gw_ota_caps_parse() reads it. */
typedef struct {
    bool declared;           /* a block (version 1 or later) */
    uint16_t version;
    uint32_t flags;
    uint16_t cfg_schema;     /* 0 below version 2 */
    uint16_t nvs_format;
    uint32_t lfs_disk;
} gw_ota_caps_info_t;

/** The block at @p block (@p len bytes from GW_OTA_CAPS_OFFSET of an image). declared is false
 *  when there is none (no magic, an unknown version 0, a size below 12 or above @p len); the
 *  version 2 fields are 0 when the block is shorter or older. */
void gw_ota_caps_parse(const uint8_t *block, size_t len, gw_ota_caps_info_t *out);

/** Issue #102: may the image replace the running one? @p nvs_encrypted: this device's NVS
 *  partition is encrypted (or may be: a migration pending). An image without NVS encryption
 *  cannot read it: it would erase it (secrets). No force overrides it. On false @p why says
 *  why. */
bool gw_ota_caps_accept(const gw_ota_caps_info_t *img, bool nvs_encrypted, char *why, size_t cap);

typedef enum {
    GW_OTA_PF_OK = 0,        /* the image reads the stored data */
    GW_OTA_PF_REFUSE,        /* it cannot: refused unless forced (@p why lists the reasons) */
    GW_OTA_PF_UNKNOWN,       /* it declares no schema (capabilities below version 2): force */
} gw_ota_pf_t;

/** Issue #144 item 3 (and #148): the pre-flight check before any write. @p own: what the running
 *  image declares; @p img: what the new image declares; @p doc: the schema report of the stored
 *  configuration (NULL or not valid: nothing to check, for example a configuration that the
 *  running image rejects or does not load). The LittleFS check refuses another major version or
 *  a lower minor version. */
gw_ota_pf_t gw_ota_preflight(const gw_ota_caps_info_t *own, const gw_ota_caps_info_t *img,
                             const gw_schema_report_t *doc, char *why, size_t cap);

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
