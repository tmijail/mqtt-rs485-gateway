/*
 * Issue #226: the text of HTTP headers and of short fields of the web UI and the REST API:
 * gw_auth_parse_basic (Authorization), gw_base64_decode, gw_auth_check_password (the stored
 * hash and the password of a request), gw_auth_csrf_check (Host, Origin, Referer and
 * X-Requested-With), gw_ota_url_ok, gw_ota_sha256_parse, gw_san_has_host, gw_wpa_pass_check and
 * gw_wpa_pmk_hex_valid.
 *
 * Input: a selector byte (bits 0-2: the function; the other bits: options), then the strings,
 * each ended by a NUL byte.
 */
#include "fuzz.h"
#include "gw_auth.h"
#include "gw_certs.h"
#include "gw_ota.h"
#include "gw_wpa.h"

/* The iterations of a stored hash: more than a few make a slow input, not a finding. */
static bool few_iterations(const char *encoded)
{
    const char *p = strchr(encoded, '$');
    if (p == NULL) {
        return true;
    }
    p++;
    size_t digits = strspn(p, "0123456789");
    return digits <= 2;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fz_t f = {data, size};
    uint8_t sel = fz_u8(&f);
    unsigned opt = sel >> 4;
    char why[96];
    why[0] = '\0';

    switch (sel & 15u) {
    case 0: {
        char *h = fz_cstr(&f);
        char user[33], pass[65];
        size_t user_cap = 1u + 2u * opt;
        if (gw_auth_parse_basic(h, user, user_cap, pass, sizeof(pass))) {
            FZ_CHECK(strlen(user) < user_cap);
            FZ_CHECK(strlen(pass) < sizeof(pass));
        }
        free(h);
        break;
    }
    case 1: {
        size_t len;
        const uint8_t *p = fz_rest(&f, &len);
        char *in = (char *)fz_dup(p, len);   /* no NUL: the length is an argument */
        size_t cap = 4u * opt;
        uint8_t *out = malloc(cap != 0 ? cap : 1);
        int n = gw_base64_decode(in, len, out, cap);
        FZ_CHECK(n >= -1 && (n < 0 || (size_t)n <= cap));
        free(out);
        free(in);
        break;
    }
    case 2: {
        char *pw = fz_cstr(&f);
        char *enc = fz_cstr(&f);
        if (few_iterations(enc)) {
            (void)gw_auth_check_password(pw, enc);
        }
        free(enc);
        free(pw);
        break;
    }
    case 3: {
        char *host = fz_cstr(&f);
        char *origin = fz_cstr(&f);
        char *referer = fz_cstr(&f);
        char *rw = fz_cstr(&f);
        (void)gw_auth_csrf_check((opt & 1u) != 0, host, (opt & 2u) != 0 ? NULL : origin,
                                 (opt & 4u) != 0 ? NULL : referer, (opt & 8u) != 0 ? NULL : rw);
        free(rw);
        free(referer);
        free(origin);
        free(host);
        break;
    }
    case 4: {
        char *url = fz_cstr(&f);
        (void)gw_ota_url_ok(url, (opt & 1u) != 0, why, sizeof(why));
        FZ_CHECK(memchr(why, 0, sizeof(why)) != NULL);
        free(url);
        break;
    }
    case 5: {
        char *hex = fz_cstr(&f);
        uint8_t sha[GW_OTA_SHA256_LEN];
        (void)gw_ota_sha256_parse(hex, sha, (opt & 1u) != 0 ? NULL : why, sizeof(why));
        FZ_CHECK(memchr(why, 0, sizeof(why)) != NULL);
        free(hex);
        break;
    }
    case 6: {
        char *sans = fz_cstr(&f);
        char *host = fz_cstr(&f);
        (void)gw_san_has_host(sans, host);
        free(host);
        free(sans);
        break;
    }
    case 7: {
        char *s = fz_cstr(&f);
        (void)gw_wpa_pass_check(s);
        (void)gw_wpa_pmk_hex_valid(s);
        free(s);
        break;
    }
    case 8: {
        /* an uploaded PEM file (PUT /api/files/<name>) */
        size_t len;
        const uint8_t *p = fz_rest(&f, &len);
        char *text = (char *)fz_dup(p, len);
        size_t off = 0, klen = 0;
        if (gw_pem_key_blocks(text, len, &off, &klen) != 0) {
            FZ_CHECK(off <= len && klen <= len - off);
        }
        free(text);
        break;
    }
    default: {
        /* a file name of the REST API, and the keys of a configuration that use it */
        char *name = fz_cstr(&f);
        (void)gw_config_file_name_ok(name);
        gw_filerefs_t refs;
        memset(&refs, 0, sizeof(refs));
        snprintf(refs.name[GW_FILEREF_MQTT_CA], sizeof(refs.name[0]), "ca.pem");
        snprintf(refs.name[GW_FILEREF_OTA_CA], sizeof(refs.name[0]), "%s", name);
        unsigned users = gw_filerefs_users(&refs, name);
        FZ_CHECK(name[0] == '\0' || (users & (1u << GW_FILEREF_OTA_CA)) != 0 ||
                 strlen(name) >= sizeof(refs.name[0]));
        free(name);
        break;
    }
    }
    return 0;
}
