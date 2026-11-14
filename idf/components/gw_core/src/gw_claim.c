#include "gw_claim.h"

#include <string.h>

#include "cJSON.h"
#include "gw_config_priv.h"
#include "gw_port.h"

void gw_claim_init(gw_claim_t *c)
{
    memset(c, 0, sizeof(*c));
}

bool gw_claim_open(gw_claim_t *c, bool password_set, uint32_t duration_ms, uint64_t now_ms)
{
    if (password_set || duration_ms == 0) {
        return false;
    }
    c->open = true;
    c->until_ms = now_ms + duration_ms;
    c->opened++;
    return true;
}

bool gw_claim_active(gw_claim_t *c, uint64_t now_ms)
{
    if (c->open && now_ms >= c->until_ms) {
        c->open = false;
    }
    return c->open;
}

uint32_t gw_claim_left_ms(const gw_claim_t *c, uint64_t now_ms)
{
    return c->open && now_ms < c->until_ms ? (uint32_t)(c->until_ms - now_ms) : 0;
}

void gw_claim_close(gw_claim_t *c)
{
    c->open = false;
}

/* The password rule: the rule of a WPA2 passphrase (issue #187, gw_wpa_pass_check). */
static gw_claim_result_t check_password(const char *pw)
{
    switch (gw_wpa_pass_check(pw)) {
    case GW_WPA_PASS_OK: return GW_CLAIM_OK;
    case GW_WPA_PASS_SHORT: return GW_CLAIM_TOO_SHORT;
    case GW_WPA_PASS_LONG: return GW_CLAIM_TOO_LONG;
    case GW_WPA_PASS_BAD_CHAR: break;
    }
    return GW_CLAIM_BAD_CHAR;
}

gw_claim_result_t gw_claim_check(gw_claim_t *c, bool password_set, bool from_ap,
                                 const char *password, uint64_t now_ms)
{
    if (password_set) {
        return GW_CLAIM_PASSWORD_SET;
    }
    if (!gw_claim_active(c, now_ms)) {
        return GW_CLAIM_CLOSED;
    }
    if (!from_ap) {
        return GW_CLAIM_NOT_AP;
    }
    return check_password(password);
}

void gw_claim_done(gw_claim_t *c)
{
    c->open = false;
    c->claimed++;
}

/* A "\u0000" escape in the text: cJSON would cut the string there, and a cut password is not the
 * password that the operator typed. Escape pairs ("\\") are skipped as one unit. */
static bool has_nul_escape(const char *json, size_t len)
{
    for (size_t i = 0; i + 1 < len; i++) {
        if (json[i] == '\\') {
            if (json[i + 1] == 'u' && i + 6 <= len && memcmp(json + i + 2, "0000", 4) == 0) {
                return true;
            }
            i++;
        }
    }
    return false;
}

gw_claim_result_t gw_claim_parse(const char *json, size_t len, char *pw, size_t cap)
{
    pw[0] = '\0';
    if (len > 0 && len <= GW_CLAIM_BODY_MAX && has_nul_escape(json, len)) {
        return GW_CLAIM_BAD_CHAR;
    }
    gw_port_json_lock();
    gw_config_json_hooks();
    cJSON *root = len > 0 && len <= GW_CLAIM_BODY_MAX ? cJSON_ParseWithLength(json, len) : NULL;
    gw_claim_result_t r = GW_CLAIM_BAD_REQUEST;
    const cJSON *item = cJSON_IsObject(root) ? root->child : NULL;
    if (item != NULL && item->next == NULL && item->string != NULL &&
        strcmp(item->string, "password") == 0 && cJSON_IsString(item)) {
        size_t n = strlen(item->valuestring);
        if (n >= cap) {
            r = GW_CLAIM_TOO_LONG;
        } else {
            memcpy(pw, item->valuestring, n + 1);
            r = GW_CLAIM_OK;
        }
    }
    cJSON_Delete(root);
    gw_port_json_unlock();
    return r;
}

const char *gw_claim_text(gw_claim_result_t r)
{
    switch (r) {
    case GW_CLAIM_OK:
        return "the web password is set";
    case GW_CLAIM_PASSWORD_SET:
        return "a web password is set: the claim window does not open again (only a factory "
               "reset removes the password)";
    case GW_CLAIM_CLOSED:
        return "no claim window is open: push the BOOT button for 2 to 10 s (only while no web "
               "password is set)";
    case GW_CLAIM_NOT_AP:
        return "the claim window accepts only a client of the access point GW-Setup-XXXXXX: "
               "connect to the access point of the gateway";
    case GW_CLAIM_BAD_REQUEST:
        return "expected {\"password\": \"<new web password>\"}";
    case GW_CLAIM_TOO_SHORT:
        return "the web password must have at least 8 characters";
    case GW_CLAIM_TOO_LONG:
        return "the web password must have at most 63 characters";
    case GW_CLAIM_BAD_CHAR:
        return "the web password must have only printable ASCII characters (a space, letters "
               "A-Z and a-z, digits and the symbols of a keyboard): it is also the WPA2 key of "
               "the access point";
    }
    return "unknown result";
}

static bool in_subnet(const uint8_t a[4], const gw_claim_if_t *i)
{
    for (int k = 0; k < 4; k++) {
        if ((a[k] & i->mask[k]) != (i->ip[k] & i->mask[k])) {
            return false;
        }
    }
    return true;
}

static bool is_zero(const uint8_t a[4])
{
    return (a[0] | a[1] | a[2] | a[3]) == 0;
}

bool gw_claim_from_ap(const uint8_t local[4], const uint8_t peer[4], const gw_claim_if_t *ap,
                      const gw_claim_if_t *others, size_t n_others)
{
    if (ap == NULL || is_zero(ap->ip) || is_zero(ap->mask) || memcmp(local, ap->ip, 4) != 0 ||
        memcmp(peer, ap->ip, 4) == 0 || !in_subnet(peer, ap)) {
        return false;
    }
    bool host_zero = true, host_ones = true;   /* the host part of the peer */
    for (int k = 0; k < 4; k++) {
        uint8_t host = (uint8_t)(peer[k] & (uint8_t)~ap->mask[k]);
        host_zero = host_zero && host == 0;
        host_ones = host_ones && host == (uint8_t)~ap->mask[k];
    }
    if (host_zero || host_ones) {
        return false;   /* the network or broadcast address (and every peer of a /31, /32) */
    }
    for (size_t i = 0; i < n_others; i++) {
        if (!is_zero(others[i].ip) && in_subnet(peer, &others[i])) {
            return false;   /* the route back to the peer is ambiguous */
        }
    }
    return true;
}
