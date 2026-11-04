#include "gw_cfgdoc.h"

#include <string.h>

#include "cJSON.h"
#include "gw_config_priv.h"

typedef enum { SECRET_MQTT = 0, SECRET_WIFI, SECRET_WEB, SECRET_COUNT } secret_t;

typedef struct {
    const char *section;
    const char *sub;      /* nested object inside section, or NULL */
    const char *path;
} secret_desc_t;

static const secret_desc_t k_secrets[SECRET_COUNT] = {
    [SECRET_MQTT] = {"mqtt", NULL, "mqtt.password"},
    [SECRET_WIFI] = {"network", "wifi", "network.wifi.password"},
    [SECRET_WEB] = {"web", NULL, "web.password"},
};

static const char *stored_value(const gw_secrets_t *s, secret_t i)
{
    switch (i) {
    case SECRET_MQTT: return s->mqtt_password;
    case SECRET_WIFI: return s->wifi_password;
    default: return s->web_password_hash;
    }
}

/* Object that holds the secret's key, optionally creating the missing parents. */
static cJSON *secret_parent(cJSON *root, secret_t i, bool create)
{
    const secret_desc_t *d = &k_secrets[i];
    cJSON *o = cJSON_GetObjectItemCaseSensitive(root, d->section);
    if (o == NULL && create) {
        o = cJSON_AddObjectToObject(root, d->section);
    }
    if (o != NULL && d->sub != NULL) {
        cJSON *sub = cJSON_GetObjectItemCaseSensitive(o, d->sub);
        if (sub == NULL && create && cJSON_IsObject(o)) {
            sub = cJSON_AddObjectToObject(o, d->sub);
        }
        o = sub;
    }
    return cJSON_IsObject(o) ? o : NULL;
}

/* cJSON_Print indents with tabs: two spaces per level, one space after ':'. */
static char *print_doc(const cJSON *root, size_t *out_len)
{
    char *raw = cJSON_Print(root);
    if (raw == NULL) {
        return NULL;
    }
    size_t n = strlen(raw);
    char *out = gw_config_alloc(2 * n + 2);
    if (out != NULL) {
        size_t o = 0;
        bool line_start = true;
        for (const char *c = raw; *c != '\0'; c++) {
            if (*c == '\t') {   /* never inside strings: cJSON escapes those as \t */
                if (line_start) {
                    out[o++] = ' ';
                }
                out[o++] = ' ';
                continue;
            }
            line_start = *c == '\n';
            out[o++] = *c;
        }
        out[o++] = '\n';
        out[o] = '\0';
        *out_len = o;
    }
    gw_config_release(raw);
    return out;
}

static bool no_memory(gw_cfg_err_t *err)
{
    memset(err, 0, sizeof(*err));
    strcpy(err->msg, "memoria insuficiente para procesar la configuración");
    return false;
}

/* Parse for the document operations, after gw_config_parse (or the depth check) accepted
 * the syntax: the same input then parses here. */
static cJSON *parse_tree(const char *json, size_t len)
{
    gw_config_json_hooks();
    return cJSON_ParseWithLength(json, len);
}

typedef enum { EFF_STORED = 0, EFF_NEW, EFF_NONE } effective_t;

bool gw_cfgdoc_apply(const char *json, size_t len, gw_cfgdoc_mode_t mode,
                     const gw_secrets_t *stored, gw_config_t *cfg, gw_cfgdoc_t *out,
                     gw_cfg_err_t *err)
{
    memset(out, 0, sizeof(*out));
    /* GW_SECRET_MASK passes every secret rule (8 characters), so the document as given
     * validates exactly like the one with the effective secrets: syntax, paths, limits. */
    if (!gw_config_parse(json, len, cfg, err)) {
        return false;
    }
    cJSON *root = parse_tree(json, len);
    if (root == NULL) {
        memset(cfg, 0, sizeof(*cfg));
        return no_memory(err);
    }

    effective_t eff[SECRET_COUNT];
    const char *value[SECRET_COUNT];
    for (int i = 0; i < SECRET_COUNT; i++) {
        cJSON *parent = secret_parent(root, (secret_t)i, false);
        cJSON *item = parent ? cJSON_GetObjectItemCaseSensitive(parent, "password") : NULL;
        const char *v = cJSON_IsString(item) ? item->valuestring : NULL;
        bool masked = v != NULL && strcmp(v, GW_SECRET_MASK) == 0;
        if (mode == GW_CFGDOC_UPDATE) {
            eff[i] = v == NULL ? EFF_NONE : masked ? EFF_STORED : EFF_NEW;
        } else {
            eff[i] = (v == NULL || v[0] == '\0' || masked) ? EFF_STORED : EFF_NEW;
            out->had_clear_secrets |= eff[i] == EFF_NEW;
        }
        if (eff[i] == EFF_NEW && v[0] == '\0') {
            eff[i] = EFF_NONE;
        }
        value[i] = eff[i] == EFF_STORED ? stored_value(stored, (secret_t)i)
                   : eff[i] == EFF_NEW  ? v
                                        : "";
        /* the effective values go to cfg and out->secrets before the tree is changed */
        if (i == SECRET_MQTT) {
            strcpy(out->secrets.mqtt_password, value[i]);    /* lengths checked by the parse */
            strcpy(cfg->mqtt.password, value[i]);
        } else if (i == SECRET_WIFI) {
            strcpy(out->secrets.wifi_password, value[i]);
            strcpy(cfg->network.wifi.password, value[i]);
        } else if (eff[i] == EFF_NEW) {
            out->new_web_password = true;   /* cfg->web.password holds it, from the parse */
        } else {
            memset(cfg->web.password, 0, sizeof(cfg->web.password));
            strcpy(out->secrets.web_password_hash, value[i]);
            strcpy(cfg->web.password_hash, value[i]);
        }
        if (item != NULL) {
            cJSON_DeleteItemFromObjectCaseSensitive(parent, "password");
        }
    }
    out->secrets_changed = out->new_web_password ||
                           strcmp(out->secrets.mqtt_password, stored->mqtt_password) != 0 ||
                           strcmp(out->secrets.wifi_password, stored->wifi_password) != 0 ||
                           strcmp(out->secrets.web_password_hash, stored->web_password_hash) != 0;

    out->doc = print_doc(root, &out->doc_len);
    bool oom = out->doc == NULL || gw_config_json_alloc_failed();
    cJSON_Delete(root);
    if (oom) {
        gw_cfgdoc_free(out);
        memset(cfg, 0, sizeof(*cfg));
        return no_memory(err);
    }
    return true;
}

void gw_cfgdoc_free(gw_cfgdoc_t *doc)
{
    gw_config_release(doc->doc);
    doc->doc = NULL;
    doc->doc_len = 0;
}

char *gw_cfgdoc_export(const char *json, size_t len, const gw_secrets_t *stored,
                       size_t *out_len, gw_cfg_err_t *err)
{
    cJSON *root = NULL;
    if (gw_json_depth(json, len) <= GW_JSON_MAX_DEPTH) {
        root = parse_tree(json, len);
    }
    if (root == NULL) {
        if (gw_config_json_alloc_failed()) {
            no_memory(err);
            return NULL;
        }
        /* gw_config_parse reports the syntax error with line and column */
        gw_config_t *scratch = gw_config_alloc(sizeof(*scratch));
        if (scratch == NULL || gw_config_parse(json, len, scratch, err)) {
            no_memory(err);
        }
        gw_config_release(scratch);
        return NULL;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        memset(err, 0, sizeof(*err));
        strcpy(err->path, "$");
        strcpy(err->msg, "$: se esperaba un objeto JSON");
        return NULL;
    }
    for (int i = 0; i < SECRET_COUNT; i++) {
        cJSON *parent = secret_parent(root, (secret_t)i, false);
        cJSON *item = parent ? cJSON_GetObjectItemCaseSensitive(parent, "password") : NULL;
        bool set = stored_value(stored, (secret_t)i)[0] != '\0' ||
                   (cJSON_IsString(item) && item->valuestring[0] != '\0');
        if (item != NULL) {
            cJSON_DeleteItemFromObjectCaseSensitive(parent, "password");
        }
        if (set) {
            parent = secret_parent(root, (secret_t)i, true);
            if (parent != NULL) {
                cJSON_AddStringToObject(parent, "password", GW_SECRET_MASK);
            }
        }
    }
    char *text = gw_config_json_alloc_failed() ? NULL : print_doc(root, out_len);
    cJSON_Delete(root);
    if (text == NULL) {
        no_memory(err);
    }
    return text;
}

void gw_cfgdoc_free_text(char *text)
{
    gw_config_release(text);
}
