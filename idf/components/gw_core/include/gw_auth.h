/*
 * Credentials for the web UI and the REST API (CFG-3, CFG-4): HTTP Basic auth parsing
 * and salted, iterated password hashes (PBKDF2-HMAC-SHA256, RFC 8018), so the device
 * never stores a web password in clear (CFG-7). Pure C, no platform calls: the caller
 * supplies the random salt.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_SHA256_LEN        32
#define GW_AUTH_SALT_LEN     16
#define GW_AUTH_ITERATIONS   2048   /* about 4000 SHA-256 blocks per check (cached, see gw_web) */

typedef struct {
    uint32_t h[8];
    uint64_t bits;
    uint8_t block[64];
    size_t used;
} gw_sha256_t;

void gw_sha256_init(gw_sha256_t *s);
void gw_sha256_update(gw_sha256_t *s, const void *data, size_t len);
void gw_sha256_final(gw_sha256_t *s, uint8_t out[GW_SHA256_LEN]);
void gw_sha256(const void *data, size_t len, uint8_t out[GW_SHA256_LEN]);

void gw_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                    uint8_t out[GW_SHA256_LEN]);

/** PBKDF2-HMAC-SHA256 (RFC 8018) producing @p out_len bytes. */
void gw_pbkdf2_sha256(const uint8_t *pw, size_t pw_len, const uint8_t *salt, size_t salt_len,
                      uint32_t iterations, uint8_t *out, size_t out_len);

/** Constant-time comparison of two buffers of length @p n. */
bool gw_auth_equal(const void *a, const void *b, size_t n);

/** Standard base64 (RFC 4648) decode, padding required. Returns the length or -1. */
int gw_base64_decode(const char *in, size_t in_len, uint8_t *out, size_t cap);

/**
 * Parse an "Authorization: Basic <base64(user:password)>" header value into
 * NUL-terminated @p user and @p pass. False if it is not Basic, not valid base64,
 * has no ':' or does not fit.
 */
bool gw_auth_parse_basic(const char *header, char *user, size_t user_cap, char *pass,
                         size_t pass_cap);

/**
 * Encode the hash of @p password as "pbkdf2-sha256$<iterations>$<salt hex>$<hash hex>"
 * (about 120 characters). False if @p cap is too small.
 */
bool gw_auth_hash_password(const char *password, const uint8_t salt[GW_AUTH_SALT_LEN],
                           uint32_t iterations, char *out, size_t cap);

/** True if @p password matches @p encoded (gw_auth_hash_password format). */
bool gw_auth_check_password(const char *password, const char *encoded);

/*
 * Issue #100: cross-site request forgery. A browser that has the Basic credentials cached sends
 * them with any request, also one a foreign page makes (a form, or a "simple" fetch without a
 * CORS preflight). Every state-changing request must therefore:
 *   - carry GW_CSRF_HEADER (any value): a foreign page can only add a custom header after a CORS
 *     preflight, and the gateway answers none;
 *   - and, when the browser says where it comes from (Origin; without it, Referer), come from
 *     this same host: scheme, host and port of the request's Host header (default ports equal
 *     to none). "Origin: null" (sandboxed frames, file: pages) is refused.
 * Clients that are not browsers (curl, scripts) send neither Origin nor Referer: the header is
 * enough for them.
 */
#define GW_CSRF_HEADER "X-Requested-With"

typedef enum {
    GW_CSRF_OK = 0,
    GW_CSRF_NO_HEADER,      /* GW_CSRF_HEADER missing or empty */
    GW_CSRF_CROSS_ORIGIN,   /* Origin or Referer of another site, or no Host to compare with */
} gw_csrf_t;

/** @p https: the scheme the request came in by. NULL or "" = header absent. */
gw_csrf_t gw_auth_csrf_check(bool https, const char *host, const char *origin,
                             const char *referer, const char *requested_with);

#ifdef __cplusplus
}
#endif
