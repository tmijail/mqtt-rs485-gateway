/* Issue #64, #205: the web server's certificate (see gw_webcert.c). */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/* #205: the PEM of the certificate (with its chain) and of its key, with the NUL. The limit of
 * an NVS string (4000 bytes with the NUL): the certificate and its key stay in NVS with the
 * secrets, as the certificate of the first boot (#64). */
#define GW_WEBCERT_PEM_MAX 4000
#define GW_WEBCERT_KEY_MAX 4000

/* The source of a certificate (NVS "web_src"). */
#define GW_WEBCERT_SRC_FIRST  "first-boot"   /* made by the gateway at its first boot */
#define GW_WEBCERT_SRC_NEW    "new"          /* made by the gateway on request (web UI) */
#define GW_WEBCERT_SRC_LOADED "loaded"       /* loaded from the web UI (the plant CA) */

typedef struct {
    char source[16];         /* one of GW_WEBCERT_SRC_*, or "self-signed" / "loaded" when
                              * the source is not stored */
    char subject[160];
    char issuer[160];
    char sans[320];          /* "DNS:gw01.local,DNS:gw01,IP:192.168.1.50" */
    char not_before[11];     /* "2025-01-01" (UTC) */
    char not_after[11];
    char fingerprint[96];    /* SHA-256 of the DER of the first certificate, "AB:CD:..." */
    char key_type[24];       /* "EC P-256", "RSA 2048" */
    unsigned chain;          /* certificates in the PEM */
    bool self_signed;        /* the issuer is the subject */
} gw_webcert_info_t;

/**
 * The device certificate and key (PEM), generated and stored in NVS the first time. A
 * certificate that waits for the restart (#205) becomes the certificate in use first. Logs the
 * SHA-256 fingerprint. False if there is none and it could not be generated. The strings live
 * for the whole run (PSRAM, allocated once).
 */
bool gw_webcert_load(const char *hostname, const char **cert, const char **key);

/**
 * #205: the certificate in use (@p pending false: the one that gw_webcert_load gave, or the
 * stored one when the server runs without HTTPS) or the one that waits for the restart
 * (@p pending true). False when there is none. Never the key.
 */
bool gw_webcert_get(bool pending, gw_webcert_info_t *out);

/**
 * #205: examine a certificate of the plant: @p cert (PEM, with its chain, NUL at cert[clen]) and
 * its unencrypted private key @p key (NUL at key[klen]) parse, and the key agrees with the
 * first certificate. With @p store, they become the certificate that waits for the restart.
 * ESP_ERR_INVALID_ARG (the reason in @p why), ESP_ERR_INVALID_SIZE (over the limits),
 * ESP_ERR_NO_MEM, or ESP_FAIL when the store in NVS fails (also when the NVS has not
 * sufficient free space: gw_webcert_nvs_need).
 */
esp_err_t gw_webcert_put(const char *cert, size_t clen, const char *key, size_t klen, bool store,
                         gw_webcert_info_t *out, char *why, size_t why_cap);

/** #205: make a new key and self-signed certificate for @p hostname; it waits for the restart. */
esp_err_t gw_webcert_new(const char *hostname, gw_webcert_info_t *out, char *why, size_t why_cap);

/** #205: discard the certificate that waits for the restart. ESP_ERR_NOT_FOUND: there is none. */
esp_err_t gw_webcert_discard(void);
