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

/* The private key @p kp (NUL at kp[klen]) agrees with the first certificate of @p cp. */
static bool pair_ok(const char *kp, size_t klen, const char *cp, size_t clen)
{
    mbedtls_pk_context *pk = gw_storage_calloc(1, sizeof(*pk));
    mbedtls_x509_crt *crt = gw_storage_calloc(1, sizeof(*crt));
    bool ok = false;
    if (pk != NULL && crt != NULL) {
        psa_crypto_init();
        mbedtls_pk_init(pk);
        mbedtls_x509_crt_init(crt);
        ok = mbedtls_pk_parse_key(pk, (const unsigned char *)kp, klen + 1, NULL, 0) == 0 &&
             mbedtls_x509_crt_parse(crt, (const unsigned char *)cp, clen + 1) == 0 &&
             mbedtls_pk_check_pair(&crt->pk, pk) == 0;
        mbedtls_pk_free(pk);
        mbedtls_x509_crt_free(crt);
    }
    gw_storage_free(pk);
    gw_storage_free(crt);
    return ok;
}

bool gw_pem_key_agrees(const char *key, const char *cert)
{
    char why[96];
    size_t klen = 0, clen = 0;
    char *kp = gw_storage_read_file(key, &klen, why, sizeof(why));
    char *cp = kp != NULL ? gw_storage_read_file(cert, &clen, why, sizeof(why)) : NULL;
    bool ok = kp != NULL && cp != NULL && pair_ok(kp, klen, cp, clen);
    if (kp != NULL) {
        memset(kp, 0, klen);   /* a private key */
    }
    gw_storage_free(kp);
    gw_storage_free(cp);
    return ok;
}

bool gw_pem_text_agrees(const char *key, size_t klen, const char *cert, size_t clen)
{
    return pair_ok(key, klen, cert, clen);
}

esp_err_t gw_pem_put(const char *name, const char *data, size_t len, gw_pem_info_t *out,
                     char *why, size_t why_cap)
{
    gw_pem_load_t f = {.name = name, .pem = data, .len = len};
    char path[GW_CFG_PATH_SIZE];
    esp_err_t r = gw_pem_load(&f, 1, false, false, path, sizeof(path), why, why_cap);
    *out = f.info;
    return r;
}

typedef struct {
    char name[GW_FILE_SIZE];
} file_name_t;

typedef struct {
    file_name_t *names;
    size_t count, total, cap;
} count_t;

static void count_one(void *ctx, const char *name, size_t size)
{
    count_t *c = ctx;
    if (c->count < c->cap) {
        snprintf(c->names[c->count].name, GW_FILE_SIZE, "%s", name);
    }
    c->count++;
    c->total += size;
}

static bool listed(const count_t *c, const char *name)
{
    for (size_t i = 0; i < c->count && i < c->cap; i++) {
        if (strcmp(c->names[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

/* The text of the pair check: the file of the load, or the stored file (read into @p *buf). */
static const char *pair_text(const gw_pem_load_t *f, size_t n, const char *name, bool in_load,
                             char **buf, size_t *len, char *why, size_t why_cap)
{
    if (in_load) {
        for (size_t i = 0; i < n; i++) {
            if (strcmp(f[i].name, name) == 0) {
                *len = f[i].len;
                return f[i].pem;
            }
        }
    }
    *buf = gw_storage_read_file(name, len, why, why_cap);
    return *buf;
}

esp_err_t gw_pem_load(gw_pem_load_t *f, size_t n, bool replace, bool dry_run, char *path,
                      size_t path_cap, char *why, size_t why_cap)
{
    path[0] = why[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        memset(&f[i].info, 0, sizeof(f[i].info));
        f[i].exists = false;
        f[i].used_by = 0;
    }
    if (n == 0 || n > 2) {
        snprintf(why, why_cap, "give one file, or a certificate and its key");
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < n; i++) {
        if (!gw_storage_user_file_name_ok(f[i].name)) {
            snprintf(why, why_cap, "'%s' is not a valid file name (1-%d characters A-Z a-z 0-9 . "
                     "_ -, not .json)", f[i].name, GW_FILE_SIZE - 1);
            return ESP_ERR_INVALID_ARG;
        }
        if (f[i].len == 0 || f[i].len > GW_PEM_FILE_MAX_BYTES) {
            snprintf(why, why_cap, "%s: %u bytes (1 to %d)", f[i].name, (unsigned)f[i].len,
                     GW_PEM_FILE_MAX_BYTES);
            return ESP_ERR_INVALID_SIZE;
        }
        gw_pem_info_text(f[i].pem, f[i].len, &f[i].info);
        snprintf(f[i].info.name, sizeof(f[i].info.name), "%s", f[i].name);
        if (f[i].info.kind == GW_PEM_OTHER) {
            snprintf(why, why_cap, "%s is not PEM: give PEM certificates or an unencrypted PEM "
                     "private key (a PEM file is text that starts with -----BEGIN)", f[i].name);
            return ESP_ERR_INVALID_ARG;
        }
    }
    /* the files of the partition: the names that exist, the limit of the number of files */
    count_t c = {.names = gw_storage_calloc(GW_PEM_FILES_MAX + 4, sizeof(file_name_t)),
                 .cap = GW_PEM_FILES_MAX + 4};
    if (c.names == NULL) {
        snprintf(why, why_cap, "no memory");
        return ESP_ERR_NO_MEM;
    }
    gw_storage_list_files(count_one, &c);
    size_t added = 0, bytes = 0;
    for (size_t i = 0; i < n; i++) {
        f[i].exists = listed(&c, f[i].name);
        added += f[i].exists ? 0 : 1;
        bytes += f[i].len;
    }
    size_t files = c.count;
    gw_storage_free(c.names);
    for (size_t i = 0; i < n; i++) {
        if (f[i].exists && !replace) {
            snprintf(why, why_cap, "/cfg/%s exists: replace it (replace=1) or give another name",
                     f[i].name);
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (files + added > GW_PEM_FILES_MAX) {
        snprintf(why, why_cap, "the gateway holds %u PEM files and the maximum is %d: remove a "
                 "file first", (unsigned)files, GW_PEM_FILES_MAX);
        return ESP_FAIL;
    }
    size_t total = 0, used = 0;
    if (gw_storage_fs_info(&total, &used) && total - used < bytes + 2 * 4096 * n) {
        snprintf(why, why_cap, "not enough free space in the LittleFS partition (%u of %u bytes "
                 "free)", (unsigned)(total - used), (unsigned)total);
        return ESP_FAIL;
    }
    /* against the saved configuration: the kind that a configured key needs, the pair check */
    gw_filerefs_t refs;
    if (!gw_storage_file_refs(&refs)) {
        memset(&refs, 0, sizeof(refs));   /* no readable document: it names no file */
    }
    gw_pemload_file_t pl[2];
    for (size_t i = 0; i < n; i++) {
        pl[i] = (gw_pemload_file_t){f[i].name, f[i].info.kind};
        f[i].used_by = gw_filerefs_users(&refs, f[i].name);
    }
    gw_pemload_plan_t plan;
    if (!gw_pemload_plan(&refs, pl, n, &plan, path, path_cap, why, why_cap)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (plan.pair) {
        char *kb = NULL, *cb = NULL;
        size_t kl = 0, cl = 0;
        char w[96] = "";
        const char *kt = pair_text(f, n, plan.key, plan.key_new, &kb, &kl, w, sizeof(w));
        const char *ct = kt != NULL ? pair_text(f, n, plan.cert, plan.cert_new, &cb, &cl, w,
                                                sizeof(w))
                                    : NULL;
        bool ok = kt != NULL && ct != NULL && pair_ok(kt, kl, ct, cl);
        if (kb != NULL) {
            memset(kb, 0, kl);   /* a private key */
        }
        gw_storage_free(kb);
        gw_storage_free(cb);
        if (!ok) {
            snprintf(path, path_cap, "%s", plan.configured ? "mqtt.tls.key_file" : "");
            if (plan.configured && plan.cert_new && !plan.key_new) {
                snprintf(why, why_cap, "the key does not agree with the certificate: the saved "
                         "configuration uses %s with %s, and the new certificate has another "
                         "public key. Load the new key too, with the name %s", plan.key,
                         plan.cert, plan.key);
            } else if (plan.configured && plan.key_new && !plan.cert_new) {
                snprintf(why, why_cap, "the key does not agree with the certificate: the saved "
                         "configuration uses %s with %s. Load the new certificate too, with the "
                         "name %s", plan.key, plan.cert, plan.cert);
            } else {
                snprintf(why, why_cap, "the key does not agree with the certificate %s%s%s",
                         plan.cert, w[0] ? ": " : "", w);
            }
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (dry_run) {
        return ESP_OK;
    }
    for (size_t i = 0; i < n; i++) {
        esp_err_t r = gw_storage_write_file(f[i].name, f[i].pem, f[i].len, replace, why, why_cap);
        if (r != ESP_OK) {
            if (i > 0) {
                size_t m = strlen(why);
                snprintf(why + m, why_cap - m, " (%s was written)", f[0].name);
            }
            return r;
        }
        ESP_LOGI(TAG, "/cfg/%s %s: %s", f[i].name, f[i].exists ? "replaced" : "loaded",
                 f[i].info.kind == GW_PEM_KEY ? "a private key" : "certificates");
    }
    return ESP_OK;
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
