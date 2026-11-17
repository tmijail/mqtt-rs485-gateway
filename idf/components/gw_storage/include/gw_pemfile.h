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
#include "gw_certs.h"
#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_PEM_CERTS_MAX   4    /* certificates of a file that the information gives */
#define GW_PEM_SUBJECT_MAX 96

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

/** True if the private key @p key (@p klen bytes, NUL at key[klen]) agrees with the first
 *  certificate of @p cert (NUL at cert[clen]). */
bool gw_pem_text_agrees(const char *key, size_t klen, const char *cert, size_t clen);

/**
 * The load of a file from a file field of the web UI: gw_pem_load() of one file, without a
 * replace. An existing file is not changed (ESP_ERR_INVALID_STATE). @p out gets what it holds.
 */
esp_err_t gw_pem_put(const char *name, const char *data, size_t len, gw_pem_info_t *out,
                     char *why, size_t why_cap);

/* One file of gw_pem_load(). */
typedef struct {
    const char *name;     /* in: the name in the gateway */
    const char *pem;      /* in: the PEM text, NUL at pem[len] */
    size_t len;
    gw_pem_info_t info;   /* out: what it holds */
    bool exists;          /* out: the load replaces a file of this name */
    unsigned used_by;     /* out: the keys of the saved configuration (gw_filerefs_users) */
} gw_pem_load_t;

/**
 * #221: the load of the card Certificates: one file, or a certificate (@p f[0]) and its key
 * (@p f[1]) together. Each file must be PEM certificates or an unencrypted PEM private key, up
 * to GW_PEM_FILE_MAX_BYTES; the partition holds GW_PEM_FILES_MAX files at most. Without
 * @p replace, an existing name is refused (ESP_ERR_INVALID_STATE). When the saved configuration
 * uses a file, the new content must be of the same kind, and the client key must still agree
 * with the client certificate (gw_pemload_plan). With @p dry_run nothing is written.
 * Errors: ESP_ERR_INVALID_ARG (the content, the name, the pair: @p path gives the JSON path of a
 * configured key, or ""), ESP_ERR_INVALID_SIZE, ESP_ERR_INVALID_STATE, ESP_ERR_NO_MEM, ESP_FAIL
 * (the number of files, the free space, a write). The reason is in @p why.
 */
esp_err_t gw_pem_load(gw_pem_load_t *f, size_t n, bool replace, bool dry_run, char *path,
                      size_t path_cap, char *why, size_t why_cap);

/**
 * Before a configuration is saved: the files that @p cfg names exist and hold what their key
 * needs (certificates for ca_file and cert_file, a private key for key_file), and the key
 * agrees with the certificate. False with the JSON path and the reason in @p err.
 */
bool gw_pem_check_config(const gw_config_t *cfg, gw_cfg_err_t *err);

#ifdef __cplusplus
}
#endif
