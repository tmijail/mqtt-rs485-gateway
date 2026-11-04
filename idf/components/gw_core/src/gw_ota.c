#include "gw_ota.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

void gw_ota_trial_init(gw_ota_trial_t *t, bool pending, uint32_t now_ms, uint32_t timeout_ms)
{
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
    t->state = to;
    t->why = why;
    return to == GW_OTA_TRIAL_CONFIRMED ? GW_OTA_CONFIRM : GW_OTA_ROLLBACK;
}

gw_ota_action_t gw_ota_trial_session_up(gw_ota_trial_t *t)
{
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

gw_ota_action_t gw_ota_trial_tick(gw_ota_trial_t *t, uint32_t now_ms)
{
    if (t->state == GW_OTA_TRIAL_PENDING && (uint32_t)(now_ms - t->start_ms) >= t->timeout_ms) {
        return decide(t, GW_OTA_TRIAL_ROLLBACK, "no Sparkplug session before the timeout");
    }
    return GW_OTA_KEEP;
}

uint32_t gw_ota_trial_left_ms(const gw_ota_trial_t *t, uint32_t now_ms)
{
    uint32_t el = now_ms - t->start_ms;
    return t->state == GW_OTA_TRIAL_PENDING && el < t->timeout_ms ? t->timeout_ms - el : 0;
}

bool gw_ota_trial_open(const gw_ota_trial_t *t)
{
    return t->state == GW_OTA_TRIAL_PENDING;
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
