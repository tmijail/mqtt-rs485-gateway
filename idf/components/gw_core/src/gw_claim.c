#include "gw_claim.h"

#include <string.h>

#include "cJSON.h"
#include "gw_config_priv.h"
#include "gw_port.h"
#include "gw_utf8.h"

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

/* The password rule: GW_CLAIM_PW_MIN characters (code points) at least, GW_CLAIM_PW_MAX bytes
 * at most, well-formed UTF-8 without control characters (C0, DEL, C1). */
static gw_claim_result_t check_password(const char *pw)
{
    size_t bytes = strlen(pw);
    if (bytes > GW_CLAIM_PW_MAX) {
        return GW_CLAIM_TOO_LONG;
    }
    size_t chars = 0;
    for (size_t i = 0; i < bytes; chars++) {
        size_t n = gw_utf8_seq(pw + i, bytes - i);
        if (n == 0) {
            return GW_CLAIM_BAD_CHAR;
        }
        unsigned long cp = gw_utf8_code(pw + i, n);
        if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) {
            return GW_CLAIM_BAD_CHAR;
        }
        i += n;
    }
    return chars < GW_CLAIM_PW_MIN ? GW_CLAIM_TOO_SHORT : GW_CLAIM_OK;
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
        return "the web password must have at most 64 bytes";
    case GW_CLAIM_BAD_CHAR:
        return "the web password must be UTF-8 text without control characters";
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
