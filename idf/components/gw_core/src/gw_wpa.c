#include "gw_wpa.h"

#include <stdio.h>
#include <string.h>

gw_wpa_pass_t gw_wpa_pass_check(const char *pw)
{
    size_t n = strlen(pw);
    if (n < GW_WPA_PASS_MIN) {
        return GW_WPA_PASS_SHORT;
    }
    if (n > GW_WPA_PASS_MAX) {
        return GW_WPA_PASS_LONG;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)pw[i];
        if (c < 0x20 || c > 0x7e) {
            return GW_WPA_PASS_BAD_CHAR;
        }
    }
    return GW_WPA_PASS_OK;
}

/* ---------- SHA-1 (FIPS 180-4) ---------- */

typedef struct {
    uint32_t h[5];
    uint64_t bits;
    uint8_t block[64];
    size_t used;
} sha1_t;

static uint32_t rol(uint32_t x, unsigned n)
{
    return (x << n) | (x >> (32u - n));
}

static void compress(uint32_t h[5], const uint8_t b[64])
{
    uint32_t w[80];
    for (unsigned i = 0; i < 16; i++) {
        w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 |
               (uint32_t)b[4 * i + 2] << 8 | (uint32_t)b[4 * i + 3];
    }
    for (unsigned i = 16; i < 80; i++) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
    for (unsigned i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (bb & c) | (~bb & d);
            k = 0x5a827999u;
        } else if (i < 40) {
            f = bb ^ c ^ d;
            k = 0x6ed9eba1u;
        } else if (i < 60) {
            f = (bb & c) | (bb & d) | (c & d);
            k = 0x8f1bbcdcu;
        } else {
            f = bb ^ c ^ d;
            k = 0xca62c1d6u;
        }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(bb, 30);
        bb = a;
        a = t;
    }
    h[0] += a;
    h[1] += bb;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

static void sha1_init(sha1_t *s)
{
    static const uint32_t iv[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u,
                                   0xc3d2e1f0u};
    memcpy(s->h, iv, sizeof(iv));
    s->bits = 0;
    s->used = 0;
}

static void sha1_update(sha1_t *s, const void *data, size_t len)
{
    const uint8_t *p = data;
    s->bits += (uint64_t)len * 8u;
    while (len > 0) {
        size_t n = 64 - s->used;
        if (n > len) {
            n = len;
        }
        memcpy(s->block + s->used, p, n);
        s->used += n;
        p += n;
        len -= n;
        if (s->used == 64) {
            compress(s->h, s->block);
            s->used = 0;
        }
    }
}

static void sha1_final(sha1_t *s, uint8_t out[GW_SHA1_LEN])
{
    uint64_t bits = s->bits;
    s->block[s->used++] = 0x80;
    if (s->used > 56) {
        memset(s->block + s->used, 0, 64 - s->used);
        compress(s->h, s->block);
        s->used = 0;
    }
    memset(s->block + s->used, 0, 56 - s->used);
    for (unsigned i = 0; i < 8; i++) {
        s->block[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    compress(s->h, s->block);
    for (unsigned i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

void gw_sha1(const void *data, size_t len, uint8_t out[GW_SHA1_LEN])
{
    sha1_t s;
    sha1_init(&s);
    sha1_update(&s, data, len);
    sha1_final(&s, out);
}

/* ---------- HMAC-SHA1 (RFC 2104) and PBKDF2 (RFC 8018) ---------- */

/* The hash states after the inner and the outer padded key, as in gw_auth.c: each HMAC of a
 * short message then costs two compressions (4096 iterations x 2 blocks of the PMK: about 16000
 * compressions). */
typedef struct {
    sha1_t inner;
    sha1_t outer;
} hmac_key_t;

static void hmac_setup(hmac_key_t *k, const uint8_t *key, size_t key_len)
{
    uint8_t kb[64] = {0};
    if (key_len > 64) {
        gw_sha1(key, key_len, kb);
    } else {
        memcpy(kb, key, key_len);
    }
    uint8_t pad[64];
    for (unsigned i = 0; i < 64; i++) {
        pad[i] = kb[i] ^ 0x36u;
    }
    sha1_init(&k->inner);
    sha1_update(&k->inner, pad, 64);
    for (unsigned i = 0; i < 64; i++) {
        pad[i] = kb[i] ^ 0x5cu;
    }
    sha1_init(&k->outer);
    sha1_update(&k->outer, pad, 64);
    memset(kb, 0, sizeof(kb));
    memset(pad, 0, sizeof(pad));
}

static void hmac_run(const hmac_key_t *k, const uint8_t *a, size_t a_len, const uint8_t *b,
                     size_t b_len, uint8_t out[GW_SHA1_LEN])
{
    sha1_t s = k->inner;
    sha1_update(&s, a, a_len);
    if (b_len > 0) {
        sha1_update(&s, b, b_len);
    }
    uint8_t ih[GW_SHA1_LEN];
    sha1_final(&s, ih);
    s = k->outer;
    sha1_update(&s, ih, sizeof(ih));
    sha1_final(&s, out);
}

void gw_hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                  uint8_t out[GW_SHA1_LEN])
{
    hmac_key_t k;
    hmac_setup(&k, key, key_len);
    hmac_run(&k, msg, msg_len, NULL, 0, out);
}

void gw_pbkdf2_sha1(const uint8_t *pw, size_t pw_len, const uint8_t *salt, size_t salt_len,
                    uint32_t iterations, uint8_t *out, size_t out_len)
{
    hmac_key_t k;
    hmac_setup(&k, pw, pw_len);
    for (uint32_t block = 1; out_len > 0; block++) {
        const uint8_t be[4] = {(uint8_t)(block >> 24), (uint8_t)(block >> 16),
                               (uint8_t)(block >> 8), (uint8_t)block};
        uint8_t u[GW_SHA1_LEN], t[GW_SHA1_LEN];
        hmac_run(&k, salt, salt_len, be, 4, u);
        memcpy(t, u, sizeof(t));
        for (uint32_t i = 1; i < iterations; i++) {
            hmac_run(&k, u, sizeof(u), NULL, 0, u);
            for (unsigned j = 0; j < GW_SHA1_LEN; j++) {
                t[j] ^= u[j];
            }
        }
        size_t n = out_len < GW_SHA1_LEN ? out_len : GW_SHA1_LEN;
        memcpy(out, t, n);
        out += n;
        out_len -= n;
        memset(u, 0, sizeof(u));
        memset(t, 0, sizeof(t));
    }
    memset(&k, 0, sizeof(k));
}

/* ---------- the PMK of WPA2-PSK ---------- */

bool gw_wpa_pmk(const char *passphrase, const char *ssid, uint8_t out[GW_WPA_PMK_LEN])
{
    size_t sl = strlen(ssid);
    if (gw_wpa_pass_check(passphrase) != GW_WPA_PASS_OK || sl == 0 || sl > GW_WPA_SSID_MAX) {
        memset(out, 0, GW_WPA_PMK_LEN);
        return false;
    }
    gw_pbkdf2_sha1((const uint8_t *)passphrase, strlen(passphrase), (const uint8_t *)ssid, sl,
                   GW_WPA_ITERATIONS, out, GW_WPA_PMK_LEN);
    return true;
}

bool gw_wpa_pmk_hex(const char *passphrase, const char *ssid, char out[GW_WPA_PMK_HEX_SIZE])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t pmk[GW_WPA_PMK_LEN];
    bool ok = gw_wpa_pmk(passphrase, ssid, pmk);
    for (unsigned i = 0; i < GW_WPA_PMK_LEN; i++) {
        out[2 * i] = hex[pmk[i] >> 4];
        out[2 * i + 1] = hex[pmk[i] & 15u];
    }
    out[ok ? 2 * GW_WPA_PMK_LEN : 0] = '\0';
    memset(pmk, 0, sizeof(pmk));
    return ok;
}

bool gw_wpa_pmk_hex_valid(const char *s)
{
    size_t i = 0;
    for (; s[i] != '\0'; i++) {
        char c = s[i];
        bool digit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!digit || i >= 2 * GW_WPA_PMK_LEN) {
            return false;
        }
    }
    return i == 2 * GW_WPA_PMK_LEN;
}

void gw_wpa_ssid(const uint8_t mac[6], char out[GW_WPA_SSID_SIZE])
{
    snprintf(out, GW_WPA_SSID_SIZE, "GW-Setup-%02X%02X%02X", mac[3], mac[4], mac[5]);
}
