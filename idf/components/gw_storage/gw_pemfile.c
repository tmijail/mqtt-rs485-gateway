#include "gw_pemfile.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "gw_storage.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

static const char *TAG = "pem";

static bool set_err(gw_cfg_err_t *err, const char *path, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static bool set_err(gw_cfg_err_t *err, const char *path, const char *fmt, ...)
{
    memset(err, 0, sizeof(*err));
    snprintf(err->path, sizeof(err->path), "%s", path);
    int n = snprintf(err->msg, sizeof(err->msg), "%s: ", path);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err->msg + n, sizeof(err->msg) - (size_t)n, fmt, ap);
    va_end(ap);
    return false;
}

/* @p pem is NUL terminated at pem[len] (gw_storage_read_file, the request body). */
static void info_of(const char *pem, size_t len, gw_pem_info_t *out)
{
    psa_crypto_init();   /* mbedTLS 4: the parse goes through PSA; idempotent */
    out->kind = GW_PEM_OTHER;
    out->count = 0;
    out->key_type[0] = '\0';
    if (strstr(pem, "PRIVATE KEY-----") != NULL) {
        mbedtls_pk_context *pk = gw_storage_calloc(1, sizeof(*pk));
        if (pk == NULL) {
            return;
        }
        mbedtls_pk_init(pk);
        if (mbedtls_pk_parse_key(pk, (const unsigned char *)pem, len + 1, NULL, 0) == 0) {
            out->kind = GW_PEM_KEY;
            psa_key_type_t t = mbedtls_pk_get_key_type(pk);
            size_t bits = mbedtls_pk_get_bitlen(pk);
            if (PSA_KEY_TYPE_IS_ECC(t)) {
                bool r1 = PSA_KEY_TYPE_ECC_GET_FAMILY(t) == PSA_ECC_FAMILY_SECP_R1;
                snprintf(out->key_type, sizeof(out->key_type), r1 ? "EC P-%u" : "EC %u bits",
                         (unsigned)bits);
            } else if (PSA_KEY_TYPE_IS_RSA(t)) {
                snprintf(out->key_type, sizeof(out->key_type), "RSA %u", (unsigned)bits);
            } else {
                snprintf(out->key_type, sizeof(out->key_type), "%u bits", (unsigned)bits);
            }
        }
        mbedtls_pk_free(pk);
        gw_storage_free(pk);
        return;
    }
    mbedtls_x509_crt *crt = gw_storage_calloc(1, sizeof(*crt));
    if (crt == NULL) {
        return;
    }
    mbedtls_x509_crt_init(crt);
    int r = mbedtls_x509_crt_parse(crt, (const unsigned char *)pem, len + 1);
    unsigned n = 0;
    for (const mbedtls_x509_crt *c = crt; r >= 0 && c != NULL && c->raw.len > 0; c = c->next) {
        if (n < GW_PEM_CERTS_MAX) {
            gw_pem_cert_t *o = &out->certs[n];
            if (mbedtls_x509_dn_gets(o->subject, sizeof(o->subject), &c->subject) < 0) {
                o->subject[0] = '\0';
            }
            snprintf(o->not_after, sizeof(o->not_after), "%04u-%02u-%02u",
                     (unsigned)c->valid_to.year % 10000u, (unsigned)c->valid_to.mon % 100u,
                     (unsigned)c->valid_to.day % 100u);
            o->ca = mbedtls_x509_crt_get_ca_istrue(c) == 1;
        }
        n++;
    }
    /* r > 0: some certificates of the file did not parse; the file is not accepted */
    if (r == 0 && n > 0) {
        out->kind = GW_PEM_CERTS;
        out->count = (uint8_t)(n > 255 ? 255 : n);
    }
    mbedtls_x509_crt_free(crt);
    gw_storage_free(crt);
}

void gw_pem_info_text(const char *pem, size_t len, gw_pem_info_t *out)
{
    out->name[0] = '\0';
    out->size = len;
    info_of(pem, len, out);
}

bool gw_pem_info(const char *name, gw_pem_info_t *out, char *why, size_t why_cap)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", name);
    size_t len;
    char *pem = gw_storage_read_file(name, &len, why, why_cap);
    if (pem == NULL) {
        return false;
    }
    out->size = len;
    info_of(pem, len, out);
    gw_storage_free(pem);
    return true;
}

bool gw_pem_key_agrees(const char *key, const char *cert)
{
    char why[96];
    size_t klen, clen;
    char *kp = gw_storage_read_file(key, &klen, why, sizeof(why));
    char *cp = kp != NULL ? gw_storage_read_file(cert, &clen, why, sizeof(why)) : NULL;
    mbedtls_pk_context *pk = gw_storage_calloc(1, sizeof(*pk));
    mbedtls_x509_crt *crt = gw_storage_calloc(1, sizeof(*crt));
    bool ok = false;
    if (kp != NULL && cp != NULL && pk != NULL && crt != NULL) {
        psa_crypto_init();
        mbedtls_pk_init(pk);
        mbedtls_x509_crt_init(crt);
        ok = mbedtls_pk_parse_key(pk, (const unsigned char *)kp, klen + 1, NULL, 0) == 0 &&
             mbedtls_x509_crt_parse(crt, (const unsigned char *)cp, clen + 1) == 0 &&
             mbedtls_pk_check_pair(&crt->pk, pk) == 0;
        mbedtls_pk_free(pk);
        mbedtls_x509_crt_free(crt);
    }
    if (kp != NULL) {
        memset(kp, 0, klen);   /* a private key */
    }
    gw_storage_free(kp);
    gw_storage_free(cp);
    gw_storage_free(pk);
    gw_storage_free(crt);
    return ok;
}

esp_err_t gw_pem_put(const char *name, const char *data, size_t len, bool replace,
                     gw_pem_info_t *out, char *why, size_t why_cap)
{
    memset(out, 0, sizeof(*out));
    if (!gw_storage_user_file_name_ok(name)) {
        snprintf(why, why_cap, "'%s' is not a valid file name (1-%d characters A-Z a-z 0-9 . _ "
                 "-, not .json)", name, GW_FILE_SIZE - 1);
        return ESP_ERR_INVALID_ARG;
    }
    gw_pem_info_text(data, len, out);
    snprintf(out->name, sizeof(out->name), "%s", name);
    if (out->kind == GW_PEM_OTHER) {
        snprintf(why, why_cap, "%s is not a PEM certificate file or an unencrypted PEM private "
                 "key", name);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t r = gw_storage_write_file(name, data, len, replace, why, why_cap);
    if (r == ESP_OK) {
        ESP_LOGI(TAG, "/cfg/%s loaded: %s", name,
                 out->kind == GW_PEM_KEY ? "a private key" : "certificates");
    }
    return r;
}

/* The file of @p path holds @p want. */
static bool check_file(const char *name, const char *path, gw_pem_kind_t want, gw_cfg_err_t *err)
{
    if (name[0] == '\0') {
        return true;
    }
    gw_pem_info_t *info = gw_storage_calloc(1, sizeof(*info));
    if (info == NULL) {
        return set_err(err, path, "no memory to read /cfg/%s", name);
    }
    char why[120];
    bool ok = gw_pem_info(name, info, why, sizeof(why));
    gw_pem_kind_t kind = info->kind;
    gw_storage_free(info);
    if (!ok) {
        return set_err(err, path, "%s", why);
    }
    if (kind != want) {
        return set_err(err, path, want == GW_PEM_KEY
                                      ? "/cfg/%s is not an unencrypted PEM private key"
                                      : "/cfg/%s is not a PEM certificate file", name);
    }
    return true;
}

bool gw_pem_check_config(const gw_config_t *cfg, gw_cfg_err_t *err)
{
    const gw_tls_cfg_t *t = &cfg->mqtt.tls;
    if (!check_file(t->ca_file, "mqtt.tls.ca_file", GW_PEM_CERTS, err) ||
        !check_file(t->cert_file, "mqtt.tls.cert_file", GW_PEM_CERTS, err) ||
        !check_file(t->key_file, "mqtt.tls.key_file", GW_PEM_KEY, err) ||
        !check_file(cfg->ota.ca_file, "ota.ca_file", GW_PEM_CERTS, err)) {
        return false;
    }
    if (t->cert_file[0] != '\0' && t->key_file[0] != '\0' &&
        !gw_pem_key_agrees(t->key_file, t->cert_file)) {
        return set_err(err, "mqtt.tls.key_file", "the key does not agree with the certificate %s",
                       t->cert_file);
    }
    return true;
}
