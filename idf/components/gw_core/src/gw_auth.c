#include "gw_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- SHA-256 (FIPS 180-4) ---------- */

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t ror(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

static void compress(uint32_t h[8], const uint8_t b[64])
{
    uint32_t w[64];
    for (unsigned i = 0; i < 16; i++) {
        w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 |
               (uint32_t)b[4 * i + 2] << 8 | (uint32_t)b[4 * i + 3];
    }
    for (unsigned i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (unsigned i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = bb;
        bb = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += bb;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

void gw_sha256_init(gw_sha256_t *s)
{
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof(iv));
    s->bits = 0;
    s->used = 0;
}

void gw_sha256_update(gw_sha256_t *s, const void *data, size_t len)
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

void gw_sha256_final(gw_sha256_t *s, uint8_t out[GW_SHA256_LEN])
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
    for (unsigned i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

void gw_sha256(const void *data, size_t len, uint8_t out[GW_SHA256_LEN])
{
    gw_sha256_t s;
    gw_sha256_init(&s);
    gw_sha256_update(&s, data, len);
    gw_sha256_final(&s, out);
}

/* ---------- HMAC-SHA256 (RFC 2104) and PBKDF2 (RFC 8018) ---------- */

/* Hash states after absorbing the inner and the outer padded key: each HMAC then costs
 * two compressions for a short message, which is what makes PBKDF2 affordable. */
typedef struct {
    gw_sha256_t inner;
    gw_sha256_t outer;
} hmac_key_t;

static void hmac_setup(hmac_key_t *k, const uint8_t *key, size_t key_len)
{
    uint8_t kb[64] = {0};
    if (key_len > 64) {
        gw_sha256(key, key_len, kb);
    } else {
        memcpy(kb, key, key_len);
    }
    uint8_t pad[64];
    for (unsigned i = 0; i < 64; i++) {
        pad[i] = kb[i] ^ 0x36u;
    }
    gw_sha256_init(&k->inner);
    gw_sha256_update(&k->inner, pad, 64);
    for (unsigned i = 0; i < 64; i++) {
        pad[i] = kb[i] ^ 0x5cu;
    }
    gw_sha256_init(&k->outer);
    gw_sha256_update(&k->outer, pad, 64);
}

static void hmac_run(const hmac_key_t *k, const uint8_t *a, size_t a_len, const uint8_t *b,
                     size_t b_len, uint8_t out[GW_SHA256_LEN])
{
    gw_sha256_t s = k->inner;
    gw_sha256_update(&s, a, a_len);
    if (b_len > 0) {
        gw_sha256_update(&s, b, b_len);
    }
    uint8_t ih[GW_SHA256_LEN];
    gw_sha256_final(&s, ih);
    s = k->outer;
    gw_sha256_update(&s, ih, sizeof(ih));
    gw_sha256_final(&s, out);
}

void gw_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                    uint8_t out[GW_SHA256_LEN])
{
    hmac_key_t k;
    hmac_setup(&k, key, key_len);
    hmac_run(&k, msg, msg_len, NULL, 0, out);
}

void gw_pbkdf2_sha256(const uint8_t *pw, size_t pw_len, const uint8_t *salt, size_t salt_len,
                      uint32_t iterations, uint8_t *out, size_t out_len)
{
    hmac_key_t k;
    hmac_setup(&k, pw, pw_len);
    for (uint32_t block = 1; out_len > 0; block++) {
        const uint8_t be[4] = {(uint8_t)(block >> 24), (uint8_t)(block >> 16),
                               (uint8_t)(block >> 8), (uint8_t)block};
        uint8_t u[GW_SHA256_LEN], t[GW_SHA256_LEN];
        hmac_run(&k, salt, salt_len, be, 4, u);
        memcpy(t, u, sizeof(t));
        for (uint32_t i = 1; i < iterations; i++) {
            hmac_run(&k, u, sizeof(u), NULL, 0, u);
            for (unsigned j = 0; j < GW_SHA256_LEN; j++) {
                t[j] ^= u[j];
            }
        }
        size_t n = out_len < GW_SHA256_LEN ? out_len : GW_SHA256_LEN;
        memcpy(out, t, n);
        out += n;
        out_len -= n;
    }
}

bool gw_auth_equal(const void *a, const void *b, size_t n)
{
    const volatile uint8_t *x = a, *y = b;
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (uint8_t)(x[i] ^ y[i]);
    }
    return diff == 0;
}

/* ---------- base64 and Basic auth ---------- */

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}

int gw_base64_decode(const char *in, size_t in_len, uint8_t *out, size_t cap)
{
    if (in_len % 4 != 0) {
        return -1;
    }
    size_t n = 0;
    for (size_t i = 0; i < in_len; i += 4) {
        int v[4];
        int pad = 0;
        for (unsigned j = 0; j < 4; j++) {
            char c = in[i + j];
            if (c == '=' && i + 4 == in_len && j >= 2) {
                v[j] = 0;
                pad++;
            } else if (pad > 0 || (v[j] = b64_value(c)) < 0) {
                return -1;
            }
        }
        uint32_t w = (uint32_t)v[0] << 18 | (uint32_t)v[1] << 12 | (uint32_t)v[2] << 6 |
                     (uint32_t)v[3];
        const uint8_t bytes[3] = {(uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w};
        for (int j = 0; j < 3 - pad; j++) {
            if (n >= cap) {
                return -1;
            }
            out[n++] = bytes[j];
        }
    }
    return (int)n;
}

bool gw_auth_parse_basic(const char *header, char *user, size_t user_cap, char *pass,
                         size_t pass_cap)
{
    if (header == NULL || strncmp(header, "Basic ", 6) != 0) {
        return false;
    }
    const char *b = header + 6;
    while (*b == ' ') {
        b++;
    }
    size_t len = strlen(b);
    while (len > 0 && b[len - 1] == ' ') {
        len--;
    }
    uint8_t buf[160];
    int n = gw_base64_decode(b, len, buf, sizeof(buf));
    if (n < 0) {
        return false;
    }
    const uint8_t *colon = memchr(buf, ':', (size_t)n);
    if (colon == NULL || memchr(buf, '\0', (size_t)n) != NULL) {
        return false;
    }
    size_t ul = (size_t)(colon - buf), pl = (size_t)n - ul - 1;
    if (ul >= user_cap || pl >= pass_cap) {
        return false;
    }
    memcpy(user, buf, ul);
    user[ul] = '\0';
    memcpy(pass, colon + 1, pl);
    pass[pl] = '\0';
    return true;
}

/* ---------- encoded hashes ---------- */

#define HASH_PREFIX "pbkdf2-sha256$"

static void to_hex(const uint8_t *in, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 15];
    }
    out[2 * n] = '\0';
}

static bool from_hex(const char *in, size_t n_chars, uint8_t *out)
{
    for (size_t i = 0; i < n_chars; i++) {
        char c = in[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0) {
            return false;
        }
        if (i % 2 == 0) {
            out[i / 2] = (uint8_t)(v << 4);
        } else {
            out[i / 2] |= (uint8_t)v;
        }
    }
    return true;
}

bool gw_auth_hash_password(const char *password, const uint8_t salt[GW_AUTH_SALT_LEN],
                           uint32_t iterations, char *out, size_t cap)
{
    uint8_t dk[GW_SHA256_LEN];
    gw_pbkdf2_sha256((const uint8_t *)password, strlen(password), salt, GW_AUTH_SALT_LEN,
                     iterations, dk, sizeof(dk));
    char salt_hex[2 * GW_AUTH_SALT_LEN + 1], dk_hex[2 * GW_SHA256_LEN + 1];
    to_hex(salt, GW_AUTH_SALT_LEN, salt_hex);
    to_hex(dk, sizeof(dk), dk_hex);
    int n = snprintf(out, cap, HASH_PREFIX "%lu$%s$%s", (unsigned long)iterations, salt_hex,
                     dk_hex);
    return n > 0 && (size_t)n < cap;
}

bool gw_auth_check_password(const char *password, const char *encoded)
{
    if (encoded == NULL || strncmp(encoded, HASH_PREFIX, strlen(HASH_PREFIX)) != 0) {
        return false;
    }
    const char *p = encoded + strlen(HASH_PREFIX);
    char *end;
    unsigned long iterations = strtoul(p, &end, 10);
    if (end == p || *end != '$' || iterations == 0 || iterations > 1000000) {
        return false;
    }
    const char *salt_hex = end + 1;
    const char *dollar = strchr(salt_hex, '$');
    if (dollar == NULL || dollar - salt_hex != 2 * GW_AUTH_SALT_LEN ||
        strlen(dollar + 1) != 2 * GW_SHA256_LEN) {
        return false;
    }
    uint8_t salt[GW_AUTH_SALT_LEN], want[GW_SHA256_LEN], got[GW_SHA256_LEN];
    if (!from_hex(salt_hex, 2 * GW_AUTH_SALT_LEN, salt) ||
        !from_hex(dollar + 1, 2 * GW_SHA256_LEN, want)) {
        return false;
    }
    gw_pbkdf2_sha256((const uint8_t *)password, strlen(password), salt, sizeof(salt),
                     (uint32_t)iterations, got, sizeof(got));
    return gw_auth_equal(got, want, sizeof(got));
}
