/*
 * #221: the PEM files next to config.json (mqtt.tls.ca_file, cert_file, key_file and
 * ota.ca_file): what each file holds, the load of a file from the web UI, and the check of the
 * files that a configuration names. mbedTLS parses the files; its memory is in PSRAM (the
 * allocator of gw_storage). A private key never leaves the gateway: only its type.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_PEM_CERTS_MAX   4    /* certificates of a file that the information gives */
#define GW_PEM_SUBJECT_MAX 96

typedef enum {
    GW_PEM_OTHER = 0,     /* not a PEM certificate or an unencrypted PEM private key */
    GW_PEM_CERTS,         /* one or more certificates */
    GW_PEM_KEY,           /* an unencrypted private key */
} gw_pem_kind_t;

typedef struct {
    char subject[GW_PEM_SUBJECT_MAX];   /* "CN=ACME Plant Root CA, O=ACME" */
    char not_after[11];                  /* "2034-03-01" (UTC) */
    bool ca;                             /* basic constraints: a CA */
} gw_pem_cert_t;

typedef struct {
    char name[GW_FILE_SIZE];
    size_t size;
    gw_pem_kind_t kind;
    uint8_t count;                       /* certificates in the file (all of them) */
    gw_pem_cert_t certs[GW_PEM_CERTS_MAX];
    char key_type[24];                   /* "EC P-256", "RSA 2048" */
} gw_pem_info_t;

/** What the file @p name holds. False (with the reason in @p why) if it cannot be read. */
bool gw_pem_info(const char *name, gw_pem_info_t *out, char *why, size_t why_cap);

/** What the text @p pem (@p len bytes, not NUL terminated) holds; out->name stays empty. */
void gw_pem_info_text(const char *pem, size_t len, gw_pem_info_t *out);

/** True if the private key in the file @p key agrees with the first certificate of @p cert. */
bool gw_pem_key_agrees(const char *key, const char *cert);

/**
 * The load of a file from the web UI: @p data must be PEM certificates or an unencrypted PEM
 * private key. Without @p replace an existing file is not changed. @p out gets what it holds.
 */
esp_err_t gw_pem_put(const char *name, const char *data, size_t len, bool replace,
                     gw_pem_info_t *out, char *why, size_t why_cap);

/**
 * Before a configuration is saved: the files that @p cfg names exist and hold what their key
 * needs (certificates for ca_file and cert_file, a private key for key_file), and the key
 * agrees with the certificate. False with the JSON path and the reason in @p err.
 */
bool gw_pem_check_config(const gw_config_t *cfg, gw_cfg_err_t *err);

#ifdef __cplusplus
}
#endif
