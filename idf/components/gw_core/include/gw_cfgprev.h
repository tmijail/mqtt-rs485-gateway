/*
 * FW-1 (issue #146): the configuration from before an update, pure C (host-tested).
 *
 * When a new image is confirmed (issue #144 item 1), the firmware "arms" the record with the
 * version of the image in the other slot: the image that ran before the update. The first
 * configuration save after that, which changes the stored document, first copies the stored
 * document to /cfg/config.prev.json ("capture"). The record then holds the version of that
 * earlier image, the schema that the earlier document needs, the time of the capture and a hash
 * of the new document.
 *
 * On a downgrade (upload, download or the "Restart into <version>" button) to the version of the
 * record, when the image cannot read the stored document (the pre-flight check, gw_ota.h):
 *   - the stored document did not change since the capture (same hash): the firmware restores
 *     config.prev.json before the restart, and the pre-flight check passes;
 *   - it changed: the refusal gives the JSON paths that differ (gw_cfgprev_diff), and force
 *     installs the image into recovery mode, where the web UI can restore the earlier document.
 *
 * The record is one NVS string (key "cfg_prev", namespace "gw"), never a raw struct:
 * "a=<armed version>;v=<version>;s=<schema>;t=<unix time>;h=<hash>". Unknown keys are ignored.
 * The secrets stay in NVS: they are not in the documents.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_CFGPREV_MAX 128       /* the NVS string, with the NUL (GW_STORAGE_STR_MAX) */
#define GW_CFGPREV_VERSION 32    /* esp_app_desc_t.version, with the NUL */
#define GW_CFGPREV_HASH_INIT 2166136261u   /* FNV-1a 32 */
#define GW_CFGPREV_TIME_MIN 1600000000     /* an earlier clock is not set (no SNTP, no RTC) */

typedef struct {
    char armed[GW_CFGPREV_VERSION];    /* "" or the version that the next change captures for */
    char version[GW_CFGPREV_VERSION];  /* "" (no capture) or the version of config.prev.json */
    uint16_t schema;                   /* the schema that config.prev.json needs */
    int64_t saved;                     /* unix time of the capture, 0: unknown */
    uint32_t hash;                     /* gw_cfgprev_hash() of the document written after it */
} gw_cfgprev_t;

/** The record as text (at most GW_CFGPREV_MAX - 1 characters; a ';' in a version becomes '_').
 *  Returns its length. */
size_t gw_cfgprev_format(const gw_cfgprev_t *p, char *out, size_t cap);

/** Parse @p s (NULL: no record); false (and *p cleared) when it holds neither an armed version
 *  nor a capture. */
bool gw_cfgprev_parse(const char *s, gw_cfgprev_t *p);

/** FNV-1a 32 of @p len bytes, continued from @p h (start with GW_CFGPREV_HASH_INIT). */
uint32_t gw_cfgprev_hash(uint32_t h, const void *data, size_t len);

/** Arm the record for @p other: the version of the image that ran before the confirmed one.
 *  Nothing (false) when @p other is empty or is @p running. Returns true when it changed. */
bool gw_cfgprev_arm(gw_cfgprev_t *p, const char *other, const char *running);

typedef enum {
    GW_CFGPREV_KEEP = 0,     /* write the new document; the record stays as it is */
    GW_CFGPREV_CAPTURE,      /* copy the stored document to config.prev.json first */
    GW_CFGPREV_DISARM,       /* the record changed (disarmed): write it, nothing to copy */
} gw_cfgprev_save_t;

/** A configuration save. @p old_valid: this image accepted the stored document, with the schema
 *  @p old_schema and the hash @p old_hash (false also when there is no stored document); @p
 *  new_hash: the hash of the new document; @p now: unix time (below GW_CFGPREV_TIME_MIN:
 *  unknown). When the record is armed and the document changes, CAPTURE (the record holds the
 *  capture, disarmed). An armed record and a stored document that this image does not accept:
 *  DISARM (it cannot tell what the earlier image read). Otherwise KEEP. */
gw_cfgprev_save_t gw_cfgprev_on_save(gw_cfgprev_t *p, bool old_valid, uint16_t old_schema,
                                     uint32_t old_hash, uint32_t new_hash, int64_t now);

typedef enum {
    GW_CFGPREV_NONE = 0,     /* no earlier document for this image: the pre-flight check as is */
    GW_CFGPREV_RESTORE,      /* restore config.prev.json before the restart */
    GW_CFGPREV_CHANGED,      /* the document changed since the capture: refuse, give the paths */
} gw_cfgprev_action_t;

/** A downgrade to the image @p version that reads up to @p schema, with the stored document of
 *  hash @p hash. NONE: no capture (absent), another version, or an earlier document that the
 *  image cannot read either. RESTORE: the same hash. CHANGED: another hash. */
gw_cfgprev_action_t gw_cfgprev_decide(const gw_cfgprev_t *p, const char *version,
                                      uint16_t schema, uint32_t hash);

/** The JSON paths that differ between the documents @p a and @p b: "mqtt.host,
 *  devices[0].tags[3].name and 2 more" in @p out. Returns the count of paths (a value that
 *  differs, a key or an array item that only one has). The keys of the secrets do not count:
 *  NVS holds the secrets, not the documents. (size_t)-1 when a document does not
 *  parse or there is no memory (then @p out says so). Takes gw_port_json_lock(). */
size_t gw_cfgprev_diff(const char *a, size_t alen, const char *b, size_t blen, char *out,
                       size_t cap);

/** "configuration from before the update: v1.2, saved on 2026-11-11 10:20 UTC" (or "saved at
 *  an unknown time"); "" when there is no capture. */
void gw_cfgprev_text(const gw_cfgprev_t *p, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
