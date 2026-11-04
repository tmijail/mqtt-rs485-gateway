#include "gw_cfgdoc.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
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

/* ---------- document printer (issue #91) ----------
 *
 * The same text as cJSON_Print with its tabs turned into two spaces per level and one space
 * after ':', written in two passes over the tree: the first counts, the second writes into a
 * single block of exactly that size. cJSON_Print grows its buffer by doubling it (a copy of
 * both while it moves) and the tab conversion needed a second buffer of twice the text: the
 * largest block a validation asked for was about twice the document. Without PSRAM that block
 * decided whether PUT /api/config answered 503. */

typedef struct {
    char *buf;    /* NULL: count only */
    size_t len;
} emit_t;

static void emit(emit_t *e, const char *s, size_t n)
{
    if (e->buf != NULL) {
        memcpy(e->buf + e->len, s, n);
    }
    e->len += n;
}

static void emit_text(emit_t *e, const char *s)
{
    emit(e, s, strlen(s));
}

static void emit_indent(emit_t *e, unsigned depth)
{
    for (unsigned i = 0; i < depth; i++) {
        emit(e, "  ", 2);
    }
}

/* print_string_ptr of cJSON: only '"', '\\' and the control characters are escaped. */
static void emit_string(emit_t *e, const char *s)
{
    emit(e, "\"", 1);
    for (const unsigned char *c = (const unsigned char *)(s != NULL ? s : ""); *c != '\0'; c++) {
        const char *esc = NULL;
        switch (*c) {
        case '"': esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\b': esc = "\\b"; break;
        case '\f': esc = "\\f"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        default: break;
        }
        if (esc != NULL) {
            emit(e, esc, 2);
        } else if (*c < 32) {
            char u[8];
            snprintf(u, sizeof(u), "\\u%04x", *c);
            emit(e, u, 6);
        } else {
            emit(e, (const char *)c, 1);
        }
    }
    emit(e, "\"", 1);
}

/* print_number of cJSON (1.7.19), C locale. */
static void emit_number(emit_t *e, const cJSON *item)
{
    char nb[32];
    double d = item->valuedouble;
    if (d != d || isinf(d)) {
        strcpy(nb, "null");
    } else if (d == (double)item->valueint) {
        snprintf(nb, sizeof(nb), "%d", item->valueint);
    } else {
        snprintf(nb, sizeof(nb), "%1.15g", d);
        double test = 0.0;
        bool read = sscanf(nb, "%lg", &test) == 1;
        double max = fabs(test) > fabs(d) ? fabs(test) : fabs(d);   /* compare_double */
        if (!read || !(fabs(test - d) <= max * DBL_EPSILON)) {
            snprintf(nb, sizeof(nb), "%1.17g", d);
        }
    }
    emit_text(e, nb);
}

/* @p depth: cJSON's printbuffer depth on entry (arrays count as a level, as in cJSON). */
static bool emit_value(emit_t *e, const cJSON *item, unsigned depth)
{
    switch (item->type & 0xFF) {
    case cJSON_NULL: emit_text(e, "null"); return true;
    case cJSON_False: emit_text(e, "false"); return true;
    case cJSON_True: emit_text(e, "true"); return true;
    case cJSON_Number: emit_number(e, item); return true;
    case cJSON_Raw:
        if (item->valuestring == NULL) {
            return false;
        }
        emit_text(e, item->valuestring);
        return true;
    case cJSON_String: emit_string(e, item->valuestring); return true;
    case cJSON_Array:
        if (depth >= CJSON_NESTING_LIMIT) {
            return false;
        }
        emit(e, "[", 1);
        for (const cJSON *c = item->child; c != NULL; c = c->next) {
            if (!emit_value(e, c, depth + 1)) {
                return false;
            }
            if (c->next != NULL) {
                emit(e, ", ", 2);
            }
        }
        emit(e, "]", 1);
        return true;
    case cJSON_Object:
        if (depth >= CJSON_NESTING_LIMIT) {
            return false;
        }
        emit(e, "{\n", 2);
        for (const cJSON *c = item->child; c != NULL; c = c->next) {
            emit_indent(e, depth + 1);
            emit_string(e, c->string);
            emit(e, ": ", 2);
            if (!emit_value(e, c, depth + 1)) {
                return false;
            }
            emit_text(e, c->next != NULL ? ",\n" : "\n");
        }
        emit_indent(e, depth);
        emit(e, "}", 1);
        return true;
    default:
        return false;
    }
}

/* Length of the document (without the NUL), written to @p buf unless NULL; 0 on failure. */
static size_t print_tree(const cJSON *root, char *buf)
{
    emit_t e = {.buf = buf, .len = 0};
    if (!emit_value(&e, root, 0)) {
        return 0;
    }
    emit(&e, "\n", 1);
    if (buf != NULL) {
        buf[e.len] = '\0';
    }
    return e.len;
}

/* The indented document plus '\n', in one block of exactly its size (+ NUL). */
static char *print_doc(const cJSON *root, size_t *out_len)
{
    size_t n = print_tree(root, NULL);
    char *out = n > 0 ? gw_config_alloc(n + 1) : NULL;
    if (out != NULL) {
        print_tree(root, out);
        *out_len = n;
    }
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

void gw_jsonfeed_init(gw_jsonfeed_t *f, char *buf, size_t cap)
{
    memset(f, 0, sizeof(*f));
    f->buf = buf;
    f->cap = cap;
    if (cap > 0) {
        buf[0] = '\0';
    }
}

bool gw_jsonfeed_add(gw_jsonfeed_t *f, const char *text)
{
    for (const char *c = text; *c != '\0' && !f->complete; c++) {
        if (!f->started) {
            if (*c != '{' && *c != '[') {
                continue;
            }
            f->started = true;
        }
        if (f->len + 1 < f->cap) {
            f->buf[f->len++] = *c;
            f->buf[f->len] = '\0';
        } else {
            f->overflow = true;
        }
        if (f->in_str) {
            if (f->esc) {
                f->esc = false;
            } else if (*c == '\\') {
                f->esc = true;
            } else if (*c == '"') {
                f->in_str = false;
            }
        } else if (*c == '"') {
            f->in_str = true;
        } else if (*c == '{' || *c == '[') {
            f->depth++;
        } else if ((*c == '}' || *c == ']') && --f->depth == 0) {
            f->complete = true;
        }
    }
    return f->complete;
}
