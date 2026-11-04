#include "gw_ota.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "gw_backoff.h"

void gw_ota_trial_init(gw_ota_trial_t *t, bool pending, uint32_t now_ms, uint32_t timeout_ms)
{
    memset(t, 0, sizeof(*t));
    t->state = pending ? GW_OTA_TRIAL_PENDING : GW_OTA_TRIAL_NONE;
    t->start_ms = now_ms;
    t->timeout_ms = timeout_ms;
    t->why = pending ? "waiting for the first Sparkplug session" : "";
}

static gw_ota_action_t decide(gw_ota_trial_t *t, gw_ota_trial_state_t to, const char *why)
{
    if (t->state != GW_OTA_TRIAL_PENDING) {
        return GW_OTA_KEEP;   /* decided once */
    }
    t->why = why;
    if (to == GW_OTA_TRIAL_CONFIRMED) {
        t->state = GW_OTA_TRIAL_CONFIRMING;   /* confirmed once the write succeeds */
        t->in_flight = true;
        return GW_OTA_CONFIRM;
    }
    t->state = to;
    return GW_OTA_ROLLBACK;
}

gw_ota_action_t gw_ota_trial_session_up(gw_ota_trial_t *t, unsigned births_missing)
{
    if (births_missing > 0) {
        /* issue #99: hosts see the node without some of its devices: not the health criterion */
        if (t->state == GW_OTA_TRIAL_PENDING) {
            t->partial = true;
            t->why = "a Sparkplug session without every DBIRTH (too large for the broker)";
        }
        return GW_OTA_KEEP;
    }
    return decide(t, GW_OTA_TRIAL_CONFIRMED, "first Sparkplug session up");
}

gw_ota_action_t gw_ota_trial_config_rejected(gw_ota_trial_t *t)
{
    return decide(t, GW_OTA_TRIAL_ROLLBACK, "it rejected the stored configuration");
}

gw_ota_action_t gw_ota_trial_commissioning(gw_ota_trial_t *t)
{
    return decide(t, GW_OTA_TRIAL_CONFIRMED, "commissioning mode: nothing to judge it by");
}

gw_ota_action_t gw_ota_trial_cannot_start(gw_ota_trial_t *t, const char *why)
{
    return decide(t, GW_OTA_TRIAL_ROLLBACK, why);
}

gw_ota_action_t gw_ota_trial_operator(gw_ota_trial_t *t, const char *why)
{
    if (t->state == GW_OTA_TRIAL_CONFIRMING) {
        t->in_flight = true;   /* retried now: the restart must not roll it back */
        return GW_OTA_CONFIRM;
    }
    return decide(t, GW_OTA_TRIAL_CONFIRMED, why);
}

bool gw_ota_trial_commit_allowed(const gw_ota_trial_t *t)
{
    return t->state != GW_OTA_TRIAL_ROLLBACK;
}

gw_ota_action_t gw_ota_trial_restart_requested(gw_ota_trial_t *t)
{
    if (t->state != GW_OTA_TRIAL_CONFIRMING) {
        return GW_OTA_KEEP;
    }
    t->in_flight = true;
    return GW_OTA_CONFIRM;
}

void gw_ota_trial_confirm_result(gw_ota_trial_t *t, bool ok, uint32_t now_ms, uint32_t rnd)
{
    if (t->state != GW_OTA_TRIAL_CONFIRMING) {
        return;   /* another caller's write succeeded first */
    }
    t->in_flight = false;
    if (ok) {
        t->state = GW_OTA_TRIAL_CONFIRMED;
        return;
    }
    t->attempts++;
    t->retry_at_ms = now_ms + gw_backoff_ms(t->attempts, GW_OTA_RETRY_MIN_MS, GW_OTA_RETRY_MAX_MS,
                                            rnd);
}

gw_ota_action_t gw_ota_trial_tick(gw_ota_trial_t *t, uint32_t now_ms)
{
    if (t->state == GW_OTA_TRIAL_PENDING && (uint32_t)(now_ms - t->start_ms) >= t->timeout_ms) {
        return decide(t, GW_OTA_TRIAL_ROLLBACK,
                      t->partial ? "no complete Sparkplug session (every DBIRTH) before the timeout"
                                 : "no Sparkplug session before the timeout");
    }
    if (t->state == GW_OTA_TRIAL_CONFIRMING && !t->in_flight &&
        (int32_t)(now_ms - t->retry_at_ms) >= 0) {
        t->in_flight = true;
        return GW_OTA_CONFIRM;
    }
    return GW_OTA_KEEP;
}

uint32_t gw_ota_trial_next_ms(const gw_ota_trial_t *t, uint32_t now_ms)
{
    if (t->state == GW_OTA_TRIAL_PENDING) {
        return gw_ota_trial_left_ms(t, now_ms);
    }
    if (t->state == GW_OTA_TRIAL_CONFIRMING && !t->in_flight) {
        int32_t d = (int32_t)(t->retry_at_ms - now_ms);
        return d > 0 ? (uint32_t)d : 0;
    }
    return GW_OTA_NO_TICK;
}

uint32_t gw_ota_trial_left_ms(const gw_ota_trial_t *t, uint32_t now_ms)
{
    uint32_t el = now_ms - t->start_ms;
    return t->state == GW_OTA_TRIAL_PENDING && el < t->timeout_ms ? t->timeout_ms - el : 0;
}

bool gw_ota_trial_open(const gw_ota_trial_t *t)
{
    return t->state == GW_OTA_TRIAL_PENDING || t->state == GW_OTA_TRIAL_CONFIRMING;
}

gw_ota_refusal_t gw_ota_refusal(const gw_ota_trial_t *t, bool transfer_running)
{
    if (transfer_running) {
        return GW_OTA_REFUSAL_QUIET;
    }
    return gw_ota_trial_open(t) ? GW_OTA_REFUSAL_IN_TRIAL : GW_OTA_REFUSAL_REPORT;
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

bool gw_ota_caps_read(const uint8_t *block, size_t len, uint32_t *flags)
{
    if (len < 12u || le32(block) != GW_OTA_CAPS_MAGIC) {
        return false;
    }
    uint16_t version = (uint16_t)(block[4] | block[5] << 8);
    uint16_t size = (uint16_t)(block[6] | block[7] << 8);
    if (version == 0 || size < 12u || size > len) {
        return false;
    }
    *flags = le32(block + 8);
    return true;
}

bool gw_ota_caps_accept(bool declared, uint32_t flags, bool nvs_encrypted, char *why, size_t cap)
{
    if (!nvs_encrypted || (declared && (flags & GW_OTA_CAP_NVS_ENCRYPTION) != 0)) {
        return true;
    }
    snprintf(why, cap, "%s (this device's NVS is encrypted)",
             declared ? "built without NVS encryption" : "declares no NVS encryption");
    return false;
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

bool gw_ota_parse_request(const char *body, size_t len, char *out, char *why, size_t cap)
{
    cJSON *root = cJSON_ParseWithLength(body, len);
    bool ok = false;
    if (root == NULL || !cJSON_IsObject(root)) {
        snprintf(why, cap, "expected a JSON object {\"url\": \"https://...\"}");
    } else if (cJSON_GetArraySize(root) != 1 || !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "url"))) {
        snprintf(why, cap, "expected exactly one key, \"url\", with a string");
    } else {
        const char *u = cJSON_GetObjectItemCaseSensitive(root, "url")->valuestring;
        if (strlen(u) >= GW_OTA_URL_MAX) {
            snprintf(why, cap, "URL longer than %d characters", GW_OTA_URL_MAX - 1);
        } else {
            strcpy(out, u);
            ok = true;
        }
    }
    cJSON_Delete(root);
    return ok;
}
