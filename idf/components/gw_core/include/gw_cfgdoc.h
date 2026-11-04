/*
 * Configuration documents with secrets (CFG-4, CFG-7).
 *
 * The secrets of the schema (mqtt.password, network.wifi.password, web.password) are kept
 * outside the JSON document, in the platform's secret store. This module merges a document
 * with the stored secrets for validation, splits it back into a document without secrets
 * plus the secrets to store, and exports a document with every set secret masked:
 *
 *   export        secret set -> GW_SECRET_MASK; never the value
 *   update (PUT)  GW_SECRET_MASK -> keep the stored value; any other string -> new value;
 *                 key absent -> no secret (the document replaces the whole configuration)
 *   file (boot)   key absent, "" or GW_SECRET_MASK -> the stored value; any other string ->
 *                 a secret in clear in the file: it is used and moved to the secret store
 *
 * web.password is stored as a hash (gw_auth.h): a new clear-text password is returned in
 * cfg->web.password for the caller to hash, together with new_web_password.
 * Pure C on cJSON; allocations go through gw_config_set_allocator.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_SECRET_MASK "********"

typedef struct {
    char mqtt_password[GW_CRED_SIZE];
    char wifi_password[GW_PSK_SIZE];
    char web_password_hash[GW_PWHASH_SIZE];   /* gw_auth_hash_password format, "" = none */
} gw_secrets_t;

typedef enum {
    GW_CFGDOC_FILE = 0,   /* the stored document, at boot */
    GW_CFGDOC_UPDATE,     /* a new document from the REST API or the shell */
} gw_cfgdoc_mode_t;

typedef struct {
    gw_secrets_t secrets;     /* secrets of the resulting configuration */
    bool secrets_changed;     /* differ from the stored ones: store them */
    bool new_web_password;    /* cfg->web.password holds a new clear-text password to hash
                               * into secrets.web_password_hash (then clear it) */
    bool had_clear_secrets;   /* FILE mode: the document carried secrets in clear */
    char *doc;                /* the document without secrets, indented (2 spaces) */
    size_t doc_len;
} gw_cfgdoc_t;

/**
 * Validate @p json (gw_config_parse, with the effective secrets) into @p cfg and split it.
 * On success @p out->doc must be released with gw_cfgdoc_free(). On failure @p err has the
 * message of gw_config_parse (JSON path or line and column) and nothing is allocated.
 */
bool gw_cfgdoc_apply(const char *json, size_t len, gw_cfgdoc_mode_t mode,
                     const gw_secrets_t *stored, gw_config_t *cfg, gw_cfgdoc_t *out,
                     gw_cfg_err_t *err);

void gw_cfgdoc_free(gw_cfgdoc_t *doc);

/**
 * Indented copy of @p json for export: every secret that is set (in @p stored, or in clear
 * in the document) becomes GW_SECRET_MASK and every other secret key is removed. Only the
 * syntax has to be valid, so a rejected configuration can still be exported and fixed.
 * Returns NULL with the reason in @p err; release with gw_cfgdoc_free_text().
 */
char *gw_cfgdoc_export(const char *json, size_t len, const gw_secrets_t *stored,
                       size_t *out_len, gw_cfg_err_t *err);

void gw_cfgdoc_free_text(char *text);

#ifdef __cplusplus
}
#endif
