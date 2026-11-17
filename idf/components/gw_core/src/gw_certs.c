#include "gw_certs.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "gw_config_priv.h"
#include "gw_port.h"

static const char *const k_path[GW_FILEREF_COUNT] = {
    "mqtt.tls.ca_file", "mqtt.tls.cert_file", "mqtt.tls.key_file", "ota.ca_file"};

const char *gw_fileref_path(gw_fileref_t r)
{
    return (unsigned)r < GW_FILEREF_COUNT ? k_path[r] : "";
}

gw_pem_kind_t gw_fileref_kind(gw_fileref_t r)
{
    return r == GW_FILEREF_MQTT_KEY ? GW_PEM_KEY : GW_PEM_CERTS;
}

static void copy_name(char *out, const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_IsObject(o) ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
    out[0] = '\0';
    if (cJSON_IsString(v) && v->valuestring != NULL && strlen(v->valuestring) < GW_FILE_SIZE) {
        memcpy(out, v->valuestring, strlen(v->valuestring) + 1);
    }
}

bool gw_filerefs_parse(const char *json, size_t len, gw_filerefs_t *out)
{
    memset(out, 0, sizeof(*out));
    gw_port_json_lock();
    gw_config_json_hooks();
    cJSON *root = cJSON_ParseWithLength(json, len);
    bool ok = cJSON_IsObject(root);
    if (ok) {
        const cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(root, "mqtt");
        const cJSON *tls = cJSON_IsObject(mqtt) ? cJSON_GetObjectItemCaseSensitive(mqtt, "tls")
                                                : NULL;
        copy_name(out->name[GW_FILEREF_MQTT_CA], tls, "ca_file");
        copy_name(out->name[GW_FILEREF_MQTT_CERT], tls, "cert_file");
        copy_name(out->name[GW_FILEREF_MQTT_KEY], tls, "key_file");
        copy_name(out->name[GW_FILEREF_OTA_CA], cJSON_GetObjectItemCaseSensitive(root, "ota"),
                  "ca_file");
    }
    cJSON_Delete(root);
    gw_port_json_unlock();
    return ok;
}

void gw_filerefs_from_config(const gw_config_t *cfg, gw_filerefs_t *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->name[GW_FILEREF_MQTT_CA], cfg->mqtt.tls.ca_file, GW_FILE_SIZE);
    memcpy(out->name[GW_FILEREF_MQTT_CERT], cfg->mqtt.tls.cert_file, GW_FILE_SIZE);
    memcpy(out->name[GW_FILEREF_MQTT_KEY], cfg->mqtt.tls.key_file, GW_FILE_SIZE);
    memcpy(out->name[GW_FILEREF_OTA_CA], cfg->ota.ca_file, GW_FILE_SIZE);
    for (unsigned i = 0; i < GW_FILEREF_COUNT; i++) {
        out->name[i][GW_FILE_SIZE - 1] = '\0';
    }
}

unsigned gw_filerefs_users(const gw_filerefs_t *refs, const char *name)
{
    unsigned m = 0;
    for (unsigned i = 0; name[0] != '\0' && i < GW_FILEREF_COUNT; i++) {
        if (strcmp(refs->name[i], name) == 0) {
            m |= 1u << i;
        }
    }
    return m;
}

/* The first occurrence of @p pat (NUL terminated) in text[from, len), or NULL. */
static const char *find(const char *text, size_t len, size_t from, const char *pat)
{
    size_t n = strlen(pat);
    for (size_t i = from; i + n <= len; i++) {
        if (memcmp(text + i, pat, n) == 0) {
            return text + i;
        }
    }
    return NULL;
}

unsigned gw_pem_key_blocks(const char *text, size_t len, size_t *off, size_t *klen)
{
    unsigned count = 0;
    size_t at = 0;
    *off = *klen = 0;
    for (;;) {
        /* "-----BEGIN PRIVATE KEY-----", "-----BEGIN EC PRIVATE KEY-----", "... RSA ..." */
        const char *b = find(text, len, at, "-----BEGIN ");
        if (b == NULL) {
            break;
        }
        const char *eol = find(text, len, (size_t)(b - text) + 11, "-----");
        if (eol == NULL) {
            break;
        }
        size_t lab = (size_t)(eol - b);   /* "-----BEGIN <label>" */
        bool key = lab >= 11 + 11 && memcmp(eol - 11, "PRIVATE KEY", 11) == 0;
        const char *e = find(text, len, (size_t)(eol - text) + 5, "-----END ");
        if (e == NULL) {
            break;
        }
        const char *ee = find(text, len, (size_t)(e - text) + 9, "-----");
        if (ee == NULL) {
            break;
        }
        size_t end = (size_t)(ee - text) + 5;
        if (end < len && text[end] == '\r') {
            end++;
        }
        if (end < len && text[end] == '\n') {
            end++;
        }
        if (key) {
            if (count++ == 0) {
                *off = (size_t)(b - text);
                *klen = end - *off;
            }
        }
        at = end;
    }
    return count;
}

static bool refuse(char *path, size_t pcap, char *why, size_t wcap, const char *p,
                   const char *fmt, ...) __attribute__((format(printf, 6, 7)));

static bool refuse(char *path, size_t pcap, char *why, size_t wcap, const char *p,
                   const char *fmt, ...)
{
    snprintf(path, pcap, "%s", p);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, wcap, fmt, ap);
    va_end(ap);
    return false;
}

bool gw_pemload_plan(const gw_filerefs_t *refs, const gw_pemload_file_t *files, size_t n,
                     gw_pemload_plan_t *plan, char *path, size_t path_cap, char *why,
                     size_t why_cap)
{
    memset(plan, 0, sizeof(*plan));
    path[0] = why[0] = '\0';
    if (n == 0 || n > 2) {
        return refuse(path, path_cap, why, why_cap, "",
                      "give one file, or a certificate and its key");
    }
    if (n == 2) {
        if (strcmp(files[0].name, files[1].name) == 0) {
            return refuse(path, path_cap, why, why_cap, "",
                          "the certificate and the key need two names (both are %s)",
                          files[0].name);
        }
        if (files[0].kind != GW_PEM_CERTS || files[1].kind != GW_PEM_KEY) {
            return refuse(path, path_cap, why, why_cap, "",
                          "give a certificate file and an unencrypted private key");
        }
    }
    for (size_t i = 0; i < n; i++) {
        unsigned m = gw_filerefs_users(refs, files[i].name);
        for (unsigned r = 0; r < GW_FILEREF_COUNT; r++) {
            if ((m & (1u << r)) && gw_fileref_kind((gw_fileref_t)r) != files[i].kind) {
                return refuse(path, path_cap, why, why_cap, k_path[r],
                              gw_fileref_kind((gw_fileref_t)r) == GW_PEM_KEY
                                  ? "the saved configuration uses %s as %s: the new file must be "
                                    "an unencrypted PEM private key"
                                  : "the saved configuration uses %s as %s: the new file must be "
                                    "PEM certificates",
                              files[i].name, k_path[r]);
            }
        }
    }
    const char *cc = refs->name[GW_FILEREF_MQTT_CERT], *ck = refs->name[GW_FILEREF_MQTT_KEY];
    bool cert_new = false, key_new = false;
    for (size_t i = 0; i < n; i++) {
        cert_new = cert_new || (cc[0] != '\0' && strcmp(files[i].name, cc) == 0);
        key_new = key_new || (ck[0] != '\0' && strcmp(files[i].name, ck) == 0);
    }
    if (cc[0] != '\0' && ck[0] != '\0' && (cert_new || key_new)) {
        *plan = (gw_pemload_plan_t){.pair = true, .cert = cc, .key = ck, .cert_new = cert_new,
                                    .key_new = key_new, .configured = true};
    } else if (n == 2) {
        *plan = (gw_pemload_plan_t){.pair = true, .cert = files[0].name, .key = files[1].name,
                                    .cert_new = true, .key_new = true};
    }
    return true;
}

size_t gw_nvs_str_entries(size_t len)
{
    return 1u + (len + 1u + GW_NVS_ENTRY_BYTES - 1u) / GW_NVS_ENTRY_BYTES;
}

size_t gw_webcert_nvs_need(size_t cert_len, size_t key_len, size_t src_len)
{
    size_t one = gw_nvs_str_entries(cert_len) + gw_nvs_str_entries(key_len) +
                 gw_nvs_str_entries(src_len);
    return 2u * one + GW_NVS_PAGE_ENTRIES;
}

static bool same_ci(const char *a, size_t an, const char *b, size_t bn)
{
    if (an != bn) {
        return false;
    }
    for (size_t i = 0; i < an; i++) {
        char x = a[i], y = b[i];
        x = (char)(x >= 'A' && x <= 'Z' ? x + 32 : x);
        y = (char)(y >= 'A' && y <= 'Z' ? y + 32 : y);
        if (x != y) {
            return false;
        }
    }
    return true;
}

bool gw_san_has_host(const char *sans, const char *host)
{
    size_t hn = strlen(host);
    if (hn == 0) {
        return false;
    }
    for (const char *s = sans; *s != '\0';) {
        const char *e = strchr(s, ',');
        size_t n = e != NULL ? (size_t)(e - s) : strlen(s);
        if (n > 4 && memcmp(s, "DNS:", 4) == 0) {
            const char *d = s + 4;
            size_t dn = n - 4;
            /* the first label of the DNS name is the hostname: <host>, <host>.local, <host>.x */
            const char *dot = memchr(d, '.', dn);
            size_t label = dot != NULL ? (size_t)(dot - d) : dn;
            if (same_ci(d, label, host, hn) && (dot == NULL || label + 1 < dn)) {
                return true;
            }
        }
        s += n;
        if (*s == ',') {
            s++;
        }
    }
    return false;
}
