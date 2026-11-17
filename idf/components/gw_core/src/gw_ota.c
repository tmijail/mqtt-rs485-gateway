#include "gw_ota.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "gw_backoff.h"
#include "gw_config_priv.h"
#include "gw_port.h"

/* ---------- issue #144 item 2: the expected mode ---------- */

gw_ota_mode_t gw_ota_expected_mode(gw_ota_mode_t from, bool force)
{
    return from == GW_OTA_MODE_NORMAL && !force ? GW_OTA_MODE_NORMAL : GW_OTA_MODE_RECOVERY;
}

bool gw_ota_mode_reached(gw_ota_mode_t expected, gw_ota_mode_t actual)
{
    if (actual == GW_OTA_MODE_NONE) {
        return false;
    }
    return expected == GW_OTA_MODE_NORMAL ? actual == GW_OTA_MODE_NORMAL : true;
}

const char *gw_ota_mode_name(gw_ota_mode_t m)
{
    switch (m) {
    case GW_OTA_MODE_RECOVERY: return "recovery";
    case GW_OTA_MODE_COMMISSIONING: return "commissioning";
    case GW_OTA_MODE_NORMAL: return "normal";
    default: return "none";
    }
}

/* ---------- issue #144 item 1: the trial of a new image ---------- */

void gw_ota_trial_init(gw_ota_trial_t *t, bool pending, gw_ota_mode_t expected,
                       uint32_t selfcheck_ms, uint32_t backstop_ms)
{
    memset(t, 0, sizeof(*t));
    t->state = pending ? GW_OTA_TRIAL_TESTING : GW_OTA_TRIAL_NONE;
    t->expected = expected == GW_OTA_MODE_NORMAL ? GW_OTA_MODE_NORMAL : GW_OTA_MODE_RECOVERY;
    t->selfcheck_ms = selfcheck_ms;
    t->backstop_ms = backstop_ms > selfcheck_ms ? backstop_ms : selfcheck_ms;
}

static gw_ota_action_t reject(gw_ota_trial_t *t, const char *prefix, const char *arg)
{
    if (t->state != GW_OTA_TRIAL_TESTING) {
        return GW_OTA_KEEP;   /* decided once */
    }
    t->state = GW_OTA_TRIAL_REJECTED;
    snprintf(t->cause, sizeof(t->cause), "%s%s", prefix, arg);
    return GW_OTA_REJECT;
}

gw_ota_action_t gw_ota_trial_mode(gw_ota_trial_t *t, gw_ota_mode_t mode, const char *detail)
{
    t->mode = mode;
    if (gw_ota_mode_reached(t->expected, mode)) {
        return GW_OTA_KEEP;
    }
    if (mode == GW_OTA_MODE_COMMISSIONING) {
        return reject(t, "configuration rejected: ", "commissioning");
    }
    return reject(t, "configuration rejected: ",
                  detail != NULL && detail[0] != '\0' ? detail : "?");
}

gw_ota_action_t gw_ota_trial_cannot_start(gw_ota_trial_t *t, const char *cause)
{
    return reject(t, "", cause);
}

static gw_ota_action_t confirm_due(gw_ota_trial_t *t, uint32_t uptime_ms)
{
    if (t->state == GW_OTA_TRIAL_CONFIRMING && !t->in_flight &&
        (int32_t)(uptime_ms - t->retry_at_ms) >= 0) {
        t->in_flight = true;
        return GW_OTA_CONFIRM;
    }
    return GW_OTA_KEEP;
}

gw_ota_action_t gw_ota_trial_check(gw_ota_trial_t *t, uint32_t uptime_ms,
                                   const gw_ota_selfcheck_t *sc)
{
    if (t->state == GW_OTA_TRIAL_CONFIRMING) {
        return confirm_due(t, uptime_ms);
    }
    if (t->state != GW_OTA_TRIAL_TESTING) {
        return GW_OTA_KEEP;
    }
    const char *item = NULL;
    if (t->mode == GW_OTA_MODE_NONE) {
        item = "mode not reached";
    } else if (!sc->net_started) {
        item = "network driver not started";
    } else if (sc->web_needed && !sc->web_alive) {
        item = "web server not alive";
    }
    if (item == NULL && uptime_ms >= t->selfcheck_ms) {
        t->state = GW_OTA_TRIAL_CONFIRMING;   /* confirmed once the write succeeds */
        t->in_flight = true;
        return GW_OTA_CONFIRM;
    }
    if (item != NULL && uptime_ms >= t->backstop_ms) {
        return reject(t, "self-check failed: ", item);
    }
    return GW_OTA_KEEP;
}

gw_ota_action_t gw_ota_trial_backstop(gw_ota_trial_t *t, const char *item)
{
    return reject(t, "self-check failed: ", item);
}

void gw_ota_trial_confirm_result(gw_ota_trial_t *t, bool ok, uint32_t uptime_ms, uint32_t rnd)
{
    if (t->state != GW_OTA_TRIAL_CONFIRMING) {
        return;
    }
    t->in_flight = false;
    if (ok) {
        t->state = GW_OTA_TRIAL_CONFIRMED;
        return;
    }
    t->attempts++;
    t->retry_at_ms = uptime_ms + gw_backoff_ms(t->attempts, GW_OTA_RETRY_MIN_MS,
                                               GW_OTA_RETRY_MAX_MS, rnd);
}

gw_ota_action_t gw_ota_trial_before_restart(gw_ota_trial_t *t)
{
    if (t->state != GW_OTA_TRIAL_CONFIRMING || t->in_flight) {
        return GW_OTA_KEEP;
    }
    t->in_flight = true;
    return GW_OTA_CONFIRM;
}

bool gw_ota_trial_open(const gw_ota_trial_t *t)
{
    return t->state == GW_OTA_TRIAL_TESTING || t->state == GW_OTA_TRIAL_CONFIRMING ||
           t->state == GW_OTA_TRIAL_REJECTED;
}

uint32_t gw_ota_trial_left_ms(const gw_ota_trial_t *t, uint32_t uptime_ms)
{
    if (t->state != GW_OTA_TRIAL_TESTING) {
        return 0;
    }
    if (uptime_ms < t->selfcheck_ms) {
        return t->selfcheck_ms - uptime_ms;
    }
    return uptime_ms < t->backstop_ms ? t->backstop_ms - uptime_ms : 0;
}

void gw_ota_trial_refusal(const gw_ota_trial_t *t, const char *version, uint32_t uptime_ms,
                          char *out, size_t cap)
{
    if (t->state == GW_OTA_TRIAL_REJECTED) {
        snprintf(out, cap, "%s was rejected (%s): restarting into the earlier firmware", version,
                 t->cause);
    } else if (t->state == GW_OTA_TRIAL_CONFIRMING) {
        snprintf(out, cap, "testing %s: not marked valid yet", version);
    } else {
        snprintf(out, cap, "testing %s: %lu s left", version,
                 (unsigned long)((gw_ota_trial_left_ms(t, uptime_ms) + 999u) / 1000u));
    }
}

static bool lower_prefix(const char *s, const char *p)
{
    for (; *p; s++, p++) {
        char c = *s >= 'A' && *s <= 'Z' ? (char)(*s - 'A' + 'a') : *s;
        if (c != *p) {
            return false;
        }
    }
    return true;
}

bool gw_ota_url_ok(const char *url, bool allow_http, char *why, size_t cap)
{
    size_t n = strlen(url);
    const char *host;
    if (n == 0) {
        snprintf(why, cap, "empty URL");
        return false;
    }
    if (n >= GW_OTA_URL_MAX) {
        snprintf(why, cap, "URL longer than %d characters", GW_OTA_URL_MAX - 1);
        return false;
    }
    if (lower_prefix(url, "https://")) {
        host = url + 8;
    } else if (lower_prefix(url, "http://")) {
        if (!allow_http) {
            snprintf(why, cap, "only https:// URLs are accepted");
            return false;
        }
        host = url + 7;
    } else {
        snprintf(why, cap, "not an https:// URL");
        return false;
    }
    for (const char *c = url; *c; c++) {
        if (*c <= ' ' || *c > '~') {
            snprintf(why, cap, "URL with a space or a non-printable character at %u",
                     (unsigned)(c - url));
            return false;
        }
    }
    if (*host == '\0' || *host == '/' || *host == ':' || *host == '?' || *host == '#') {
        snprintf(why, cap, "URL without a host");
        return false;
    }
    if (strchr(host, '@') != NULL && (strchr(host, '/') == NULL || strchr(host, '@') < strchr(host, '/'))) {
        snprintf(why, cap, "credentials in the URL are not accepted");
        return false;
    }
    return true;
}


static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

void gw_ota_caps_parse(const uint8_t *block, size_t len, gw_ota_caps_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (len < 12u || le32(block) != GW_OTA_CAPS_MAGIC) {
        return;
    }
    uint16_t version = le16(block + 4);
    uint16_t size = le16(block + 6);
    if (version == 0 || size < 12u || size > len) {
        return;
    }
    out->declared = true;
    out->version = version;
    out->flags = le32(block + 8);
    if (version >= 2u && size >= 20u) {
        out->cfg_schema = le16(block + 12);
        out->nvs_format = le16(block + 14);
        out->lfs_disk = le32(block + 16);
    }
}

bool gw_ota_caps_accept(const gw_ota_caps_info_t *img, bool nvs_encrypted, char *why, size_t cap)
{
    if (!nvs_encrypted || (img->declared && (img->flags & GW_OTA_CAP_NVS_ENCRYPTION) != 0)) {
        return true;
    }
    snprintf(why, cap, "%s (this device's NVS is encrypted)",
             img->declared ? "built without NVS encryption" : "declares no NVS encryption");
    return false;
}

/* Appends "; " between the reasons. */
static int add(char *why, size_t cap, int n)
{
    if (n > 0 && (size_t)n + 2 < cap) {
        why[n++] = ';';
        why[n++] = ' ';
        why[n] = '\0';
    }
    return n;
}

gw_ota_pf_t gw_ota_preflight(const gw_ota_caps_info_t *own, const gw_ota_caps_info_t *img,
                             const gw_schema_report_t *doc, char *why, size_t cap)
{
    why[0] = '\0';
    if (!img->declared || img->version < 2u) {
        snprintf(why, cap, "the image declares no configuration schema (capabilities version "
                 "%u, built before the pre-flight check): it may not read the stored data",
                 (unsigned)img->version);
        return GW_OTA_PF_UNKNOWN;
    }
    int n = 0;
    if (doc != NULL && doc->valid && !gw_schema_fits(doc, img->cfg_schema, why, cap)) {
        n = (int)strlen(why);
    }
    unsigned own_page = own->nvs_format >> 8, img_page = img->nvs_format >> 8;
    unsigned own_rec = own->nvs_format & 0xFFu, img_rec = img->nvs_format & 0xFFu;
    if ((img_page < own_page || img_rec < own_rec) && (size_t)n < cap) {
        n = add(why, cap, n);
        n += snprintf(why + n, cap - (size_t)n, "NVS format %u.%u, this firmware writes %u.%u",
                      img_page, img_rec, own_page, own_rec);
    }
    unsigned own_major = own->lfs_disk >> 16, img_major = img->lfs_disk >> 16;
    unsigned own_minor = own->lfs_disk & 0xFFFFu, img_minor = img->lfs_disk & 0xFFFFu;
    if ((img_major != own_major || img_minor < own_minor) && (size_t)n < cap) {
        n = add(why, cap, n);
        n += snprintf(why + n, cap - (size_t)n,
                      "LittleFS disk version %u.%u, this firmware writes %u.%u", img_major,
                      img_minor, own_major, own_minor);
    }
    return n > 0 ? GW_OTA_PF_REFUSE : GW_OTA_PF_OK;
}

void gw_ota_progress_init(gw_ota_progress_t *p, uint32_t total, uint32_t now_ms)
{
    memset(p, 0, sizeof(*p));
    p->total = total;
    p->reported_ms = now_ms;
}

bool gw_ota_progress_update(gw_ota_progress_t *p, uint32_t done, uint32_t now_ms, uint8_t *pct)
{
    p->done = done;
    uint8_t v = 0;
    if (p->total != 0) {
        uint64_t x = (uint64_t)(done > p->total ? p->total : done) * 100u / p->total;
        v = (uint8_t)x;
    }
    *pct = v;
    bool due;
    if (p->total != 0) {
        due = !p->any || v >= p->reported_pct + 10u || (v == 100u && p->reported_pct != 100u);
    } else {
        due = !p->any || (uint32_t)(now_ms - p->reported_ms) >= 5000u;
    }
    if (due) {
        p->any = true;
        p->reported_pct = v;
        p->reported_ms = now_ms;
    }
    return due;
}

/* Issue #106: under the JSON lock, with the configuration allocator's hooks (the same ones every
 * other cJSON use installs), so it cannot change them, or the failure flag, under another task's
 * validation. */
/* ---------- issue #215: the SHA-256 that the user gives ---------- */

static int hex_digit(char c)
{
    return c >= '0' && c <= '9' ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}

bool gw_ota_sha256_parse(const char *hex, uint8_t out[GW_OTA_SHA256_LEN], char *why, size_t cap)
{
    size_t n = hex != NULL ? strlen(hex) : 0;
    bool ok = n == 2u * GW_OTA_SHA256_LEN;
    for (size_t i = 0; ok && i < n; i++) {
        ok = hex_digit(hex[i]) >= 0;
    }
    if (!ok) {
        if (why != NULL) {
            snprintf(why, cap, "expected 64 hexadecimal characters (0-9, a-f), got %s%u%s",
                     n == 2u * GW_OTA_SHA256_LEN ? "other characters in " : "", (unsigned)n,
                     n == 2u * GW_OTA_SHA256_LEN ? "" : " characters");
        }
        return false;
    }
    for (size_t i = 0; i < GW_OTA_SHA256_LEN; i++) {
        out[i] = (uint8_t)((hex_digit(hex[2 * i]) << 4) | hex_digit(hex[2 * i + 1]));
    }
    return true;
}

void gw_ota_sha256_hex(const uint8_t sha[GW_OTA_SHA256_LEN], char out[GW_OTA_SHA256_HEX])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < GW_OTA_SHA256_LEN; i++) {
        out[2 * i] = digits[sha[i] >> 4];
        out[2 * i + 1] = digits[sha[i] & 0x0Fu];
    }
    out[2 * GW_OTA_SHA256_LEN] = '\0';
}

bool gw_ota_parse_request(const char *body, size_t len, char *out, bool *force,
                          uint8_t sha[GW_OTA_SHA256_LEN], bool *has_sha, char *why, size_t cap)
{
    gw_port_json_lock();
    gw_config_json_hooks();
    cJSON *root = cJSON_ParseWithLength(body, len);
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(root, "force");
    const cJSON *h = cJSON_GetObjectItemCaseSensitive(root, "sha256");
    bool ok = false;
    *has_sha = false;
    if (root == NULL && gw_config_json_alloc_failed()) {
        snprintf(why, cap, "out of memory parsing the request");
    } else if (root == NULL || !cJSON_IsObject(root)) {
        snprintf(why, cap, "expected a JSON object {\"url\": \"https://...\"}");
    } else if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "url")) ||
               cJSON_GetArraySize(root) != 1 + (f != NULL ? 1 : 0) + (h != NULL ? 1 : 0)) {
        snprintf(why, cap, "expected the key \"url\" with a string, and only \"sha256\" and "
                 "\"force\" besides");
    } else if (f != NULL && !cJSON_IsBool(f)) {
        snprintf(why, cap, "\"force\" must be true or false");
    } else if (h != NULL && !cJSON_IsString(h)) {
        snprintf(why, cap, "\"sha256\" must be a string");
    } else {
        *force = cJSON_IsTrue(f);
        const char *u = cJSON_GetObjectItemCaseSensitive(root, "url")->valuestring;
        char reason[80];
        if (strlen(u) >= GW_OTA_URL_MAX) {
            snprintf(why, cap, "URL longer than %d characters", GW_OTA_URL_MAX - 1);
        } else if (h != NULL && h->valuestring[0] != '\0' &&
                   !gw_ota_sha256_parse(h->valuestring, sha, reason, sizeof(reason))) {
            snprintf(why, cap, "\"sha256\": %s", reason);
        } else {
            *has_sha = h != NULL && h->valuestring[0] != '\0';
            strcpy(out, u);
            ok = true;
        }
    }
    cJSON_Delete(root);
    gw_port_json_unlock();
    return ok;
}
