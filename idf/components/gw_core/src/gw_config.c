#include "gw_config.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/* ---------- parser memory ---------- */

static void *(*s_alloc)(size_t) = malloc;
static void (*s_release)(void *) = free;
static bool s_alloc_failed;

static void *tracked_alloc(size_t size)
{
    void *p = s_alloc(size);
    if (p == NULL) {
        s_alloc_failed = true;
    }
    return p;
}

void gw_config_set_allocator(void *(*alloc)(size_t size), void (*release)(void *ptr))
{
    s_alloc = alloc;
    s_release = release;
}

/* ---------- error reporting ---------- */

static bool fail(gw_cfg_err_t *err, const char *path, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static bool fail(gw_cfg_err_t *err, const char *path, const char *fmt, ...)
{
    int n = snprintf(err->msg, sizeof(err->msg), "%s: ", path);
    if (n < 0 || (size_t)n >= sizeof(err->msg)) {
        return false;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err->msg + n, sizeof(err->msg) - (size_t)n, fmt, ap);
    va_end(ap);
    return false;
}

static void join(char *out, size_t cap, const char *path, const char *key)
{
    snprintf(out, cap, "%s%s%s", path, path[0] != '\0' ? "." : "", key);
}

/* 1-based line and column (in characters, not UTF-8 bytes) of @p at in @p json. */
static void line_col(const char *json, const char *at, unsigned *line, unsigned *col)
{
    *line = 1;
    *col = 1;
    for (const char *c = json; c < at; c++) {
        if (*c == '\n') {
            (*line)++;
            *col = 1;
        } else if (((unsigned char)*c & 0xC0u) != 0x80u) {
            (*col)++;
        }
    }
}

static bool fail_at(gw_cfg_err_t *err, const char *json, const char *at, const char *what)
{
    unsigned line, col;
    line_col(json, at, &line, &col);
    snprintf(err->msg, sizeof(err->msg), "JSON mal formado en la línea %u, columna %u: %s", line,
             col, what);
    return false;
}

/* ---------- generic typed getters ---------- */

/* Every object of the schema goes through here: unknown keys (typos) and duplicated
 * keys (cJSON keeps both and lookups see only the first) are rejected. */
static bool check_keys(const cJSON *obj, const char *path, const char *const *allowed, size_t n,
                       gw_cfg_err_t *err)
{
    if (!cJSON_IsObject(obj)) {
        return fail(err, path[0] != '\0' ? path : "$", "se esperaba un objeto JSON");
    }
    for (const cJSON *it = obj->child; it != NULL; it = it->next) {
        char p[96];
        join(p, sizeof(p), path, it->string);
        bool known = false;
        for (size_t i = 0; i < n && !known; i++) {
            known = strcmp(it->string, allowed[i]) == 0;
        }
        if (!known) {
            return fail(err, p, "clave desconocida");
        }
        for (const cJSON *prev = obj->child; prev != it; prev = prev->next) {
            if (strcmp(prev->string, it->string) == 0) {
                return fail(err, p, "clave duplicada");
            }
        }
    }
    return true;
}

static bool get_bool(const cJSON *obj, const char *key, const char *path, bool def, bool *out,
                     gw_cfg_err_t *err)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v == NULL) {
        *out = def;
        return true;
    }
    if (!cJSON_IsBool(v)) {
        char p[96];
        join(p, sizeof(p), path, key);
        return fail(err, p, "se esperaba true o false");
    }
    *out = cJSON_IsTrue(v);
    return true;
}

static bool get_int(const cJSON *obj, const char *key, const char *path, bool required, long def,
                    long min, long max, long *out, gw_cfg_err_t *err)
{
    char p[96];
    join(p, sizeof(p), path, key);
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v == NULL) {
        if (required) {
            return fail(err, p, "campo obligatorio ausente");
        }
        *out = def;
        return true;
    }
    if (!cJSON_IsNumber(v) || v->valuedouble != floor(v->valuedouble)) {
        return fail(err, p, "se esperaba un número entero");
    }
    if (v->valuedouble < (double)min || v->valuedouble > (double)max) {
        return fail(err, p, "valor %.0f fuera de rango [%ld, %ld]", v->valuedouble, min, max);
    }
    *out = (long)v->valuedouble;
    return true;
}

static bool get_double(const cJSON *obj, const char *key, const char *path, double def, double *out,
                       gw_cfg_err_t *err)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v == NULL) {
        *out = def;
        return true;
    }
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble)) {
        char p[96];
        join(p, sizeof(p), path, key);
        return fail(err, p, "se esperaba un número");
    }
    *out = v->valuedouble;
    return true;
}

static bool get_string(const cJSON *obj, const char *key, const char *path, bool required,
                       const char *def, char *out, size_t cap, gw_cfg_err_t *err)
{
    char p[96];
    join(p, sizeof(p), path, key);
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    const char *s = def;
    if (v == NULL) {
        if (required) {
            return fail(err, p, "campo obligatorio ausente");
        }
    } else if (!cJSON_IsString(v)) {
        return fail(err, p, "se esperaba un texto");
    } else {
        s = v->valuestring;
    }
    size_t len = strlen(s);
    if (len >= cap) {
        return fail(err, p, "texto demasiado largo (%zu caracteres, máximo %zu)", len, cap - 1);
    }
    memcpy(out, s, len + 1);
    return true;
}

static bool get_enum(const cJSON *obj, const char *key, const char *path, bool required, int def,
                     const char *const *names, size_t n, int *out, gw_cfg_err_t *err)
{
    char p[96];
    join(p, sizeof(p), path, key);
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v == NULL) {
        if (required) {
            return fail(err, p, "campo obligatorio ausente");
        }
        *out = def;
        return true;
    }
    if (cJSON_IsString(v)) {
        for (size_t i = 0; i < n; i++) {
            if (strcmp(v->valuestring, names[i]) == 0) {
                *out = (int)i;
                return true;
            }
        }
    }
    char allowed[96] = "";
    for (size_t i = 0; i < n; i++) {
        strncat(allowed, names[i], sizeof(allowed) - strlen(allowed) - 1);
        if (i + 1 < n) {
            strncat(allowed, "|", sizeof(allowed) - strlen(allowed) - 1);
        }
    }
    return fail(err, p, "valor no válido (se esperaba %s)", allowed);
}

static bool parse_ipv4(const char *s, uint8_t out[4])
{
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 ||
        d > 255) {
        return false;
    }
    out[0] = (uint8_t)a;
    out[1] = (uint8_t)b;
    out[2] = (uint8_t)c;
    out[3] = (uint8_t)d;
    return true;
}

static bool get_ipv4(const cJSON *obj, const char *key, const char *path, bool required,
                     uint8_t out[4], gw_cfg_err_t *err)
{
    char p[96];
    join(p, sizeof(p), path, key);
    char buf[24];
    if (!get_string(obj, key, path, required, "", buf, sizeof(buf), err)) {
        return false;
    }
    if (buf[0] == '\0') {
        if (required) {
            return fail(err, p, "dirección IPv4 obligatoria con dhcp=false");
        }
        memset(out, 0, 4);
        return true;
    }
    if (!parse_ipv4(buf, out)) {
        return fail(err, p, "'%s' no es una dirección IPv4 válida", buf);
    }
    return true;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Sparkplug identifiers become MQTT topic levels: no '/', '+', '#', no control chars. */
static bool check_topic_id(const char *s, const char *path, bool allow_empty, gw_cfg_err_t *err)
{
    if (s[0] == '\0') {
        return allow_empty ? true : fail(err, path, "no puede estar vacío");
    }
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        if (*c == '/' || *c == '+' || *c == '#' || *c < 0x20 || *c == 0x7f) {
            return fail(err, path, "carácter no permitido '%c' (prohibidos: / + # y de control)",
                        *c >= 0x20 ? *c : '?');
        }
    }
    return true;
}

/* ---------- sections ---------- */

static const char *const k_area_names[] = {"coil", "discrete", "holding", "input"};
static const char *const k_type_names[] = {"bool", "uint16", "int16", "uint32", "int32", "float32"};
static const char *const k_word_names[] = {"big", "little"};
static const char *const k_parity_names[] = {"none", "even", "odd"};

/* dhcp/ip/netmask/gateway/dns of one interface; the addresses are required with dhcp=false. */
static bool parse_ipv4_keys(const cJSON *o, const char *path, bool *dhcp, uint8_t ip[4],
                            uint8_t netmask[4], uint8_t gateway[4], uint8_t dns[4],
                            gw_cfg_err_t *err)
{
    if (!get_bool(o, "dhcp", path, true, dhcp, err)) {
        return false;
    }
    bool st = !*dhcp;
    return get_ipv4(o, "ip", path, st, ip, err) && get_ipv4(o, "netmask", path, st, netmask, err) &&
           get_ipv4(o, "gateway", path, st, gateway, err) &&
           get_ipv4(o, "dns", path, false, dns, err);
}

static bool check_wifi_password(const char *pw)
{
    size_t n = strlen(pw);
    if (n == 0 || (n >= 8 && n <= 63)) {
        return true;
    }
    if (n != 64) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (hex_digit(pw[i]) < 0) {
            return false;
        }
    }
    return true;
}

static bool parse_wifi_section(const cJSON *net, gw_config_t *cfg, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"enabled", "ssid", "password", "role", "failback_s",
                                       "dhcp", "ip", "netmask", "gateway", "dns"};
    static const char *const roles[] = {"backup", "primary"};
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(net, "wifi");
    static const cJSON empty = {.type = cJSON_Object};
    if (o == NULL) {
        o = &empty;
    }
    gw_wifi_cfg_t *w = &cfg->network.wifi;
    int role;
    long failback;
    if (!check_keys(o, "network.wifi", keys, 10, err) ||
        !get_bool(o, "enabled", "network.wifi", false, &w->enabled, err) ||
        !get_string(o, "ssid", "network.wifi", false, "", w->ssid, sizeof(w->ssid), err) ||
        !get_string(o, "password", "network.wifi", false, "", w->password, sizeof(w->password),
                    err) ||
        !get_enum(o, "role", "network.wifi", false, GW_WIFI_BACKUP, roles, 2, &role, err) ||
        !get_int(o, "failback_s", "network.wifi", false, 30, 0, 3600, &failback, err) ||
        !parse_ipv4_keys(o, "network.wifi", &w->ipv4.dhcp, w->ipv4.ip, w->ipv4.netmask,
                         w->ipv4.gateway, w->ipv4.dns, err)) {
        return false;
    }
    w->role = (gw_wifi_role_t)role;
    w->failback_s = (uint16_t)failback;
    if (w->enabled && w->ssid[0] == '\0') {
        return fail(err, "network.wifi.ssid", "obligatorio con enabled=true");
    }
    if (!check_wifi_password(w->password)) {
        return fail(err, "network.wifi.password",
                    "se esperaban 8 a 63 caracteres (WPA2), 64 dígitos hexadecimales o vacío "
                    "(red abierta)");
    }
    return true;
}

static bool parse_network(const cJSON *root, gw_config_t *cfg, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"dhcp", "ip", "netmask", "gateway", "dns", "wifi"};
    static const cJSON empty = {.type = cJSON_Object};
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, "network");
    if (o == NULL) {
        o = &empty;
    }
    return check_keys(o, "network", keys, 6, err) &&
           parse_ipv4_keys(o, "network", &cfg->network.dhcp, cfg->network.ip,
                           cfg->network.netmask, cfg->network.gateway, cfg->network.dns, err) &&
           parse_wifi_section(o, cfg, err);
}

static bool parse_ntp(const cJSON *root, gw_config_t *cfg, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"server"};
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, "ntp");
    if (o == NULL) {
        strcpy(cfg->ntp.server, "pool.ntp.org");
        return true;
    }
    if (!check_keys(o, "ntp", keys, 1, err) ||
        !get_string(o, "server", "ntp", false, "pool.ntp.org", cfg->ntp.server,
                    sizeof(cfg->ntp.server), err)) {
        return false;
    }
    return cfg->ntp.server[0] != '\0' ? true : fail(err, "ntp.server", "no puede estar vacío");
}

static bool parse_mqtt(const cJSON *root, gw_config_t *cfg, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"host", "port", "client_id", "username", "password",
                                       "keepalive_s"};
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, "mqtt");
    if (o == NULL) {
        return fail(err, "mqtt", "sección obligatoria ausente");
    }
    long port, ka;
    if (!check_keys(o, "mqtt", keys, 6, err) ||
        !get_string(o, "host", "mqtt", true, "", cfg->mqtt.host, sizeof(cfg->mqtt.host), err) ||
        !get_int(o, "port", "mqtt", false, 1883, 1, 65535, &port, err) ||
        !get_string(o, "client_id", "mqtt", false, "", cfg->mqtt.client_id,
                    sizeof(cfg->mqtt.client_id), err) ||
        !get_string(o, "username", "mqtt", false, "", cfg->mqtt.username,
                    sizeof(cfg->mqtt.username), err) ||
        !get_string(o, "password", "mqtt", false, "", cfg->mqtt.password,
                    sizeof(cfg->mqtt.password), err) ||
        !get_int(o, "keepalive_s", "mqtt", false, 30, 5, 3600, &ka, err)) {
        return false;
    }
    if (cfg->mqtt.host[0] == '\0') {
        return fail(err, "mqtt.host", "no puede estar vacío");
    }
    cfg->mqtt.port = (uint16_t)port;
    cfg->mqtt.keepalive_s = (uint16_t)ka;
    return true;
}

static bool parse_sparkplug(const cJSON *root, gw_config_t *cfg, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"group_id", "edge_node_id", "primary_host_id"};
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, "sparkplug");
    if (o == NULL) {
        return fail(err, "sparkplug", "sección obligatoria ausente");
    }
    return check_keys(o, "sparkplug", keys, 3, err) &&
           get_string(o, "group_id", "sparkplug", true, "", cfg->sparkplug.group_id,
                      sizeof(cfg->sparkplug.group_id), err) &&
           get_string(o, "edge_node_id", "sparkplug", true, "", cfg->sparkplug.edge_node_id,
                      sizeof(cfg->sparkplug.edge_node_id), err) &&
           get_string(o, "primary_host_id", "sparkplug", false, "",
                      cfg->sparkplug.primary_host_id, sizeof(cfg->sparkplug.primary_host_id),
                      err) &&
           check_topic_id(cfg->sparkplug.group_id, "sparkplug.group_id", false, err) &&
           check_topic_id(cfg->sparkplug.edge_node_id, "sparkplug.edge_node_id", false, err) &&
           check_topic_id(cfg->sparkplug.primary_host_id, "sparkplug.primary_host_id", true, err);
}

static bool parse_rs485(const cJSON *root, gw_config_t *cfg, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"baud", "parity", "stop_bits", "response_timeout_ms",
                                       "retries", "inter_frame_delay_ms"};
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, "rs485");
    static const cJSON empty = {.type = cJSON_Object};
    if (o == NULL) {
        o = &empty;
    }
    long baud, stop, tout, retries, ifd;
    int parity;
    if (!check_keys(o, "rs485", keys, 6, err) ||
        !get_int(o, "baud", "rs485", false, 9600, 1200, 115200, &baud, err) ||
        !get_enum(o, "parity", "rs485", false, GW_PARITY_NONE, k_parity_names, 3, &parity, err) ||
        !get_int(o, "stop_bits", "rs485", false, 1, 1, 2, &stop, err) ||
        !get_int(o, "response_timeout_ms", "rs485", false, 200, 10, 5000, &tout, err) ||
        !get_int(o, "retries", "rs485", false, 2, 0, 5, &retries, err) ||
        !get_int(o, "inter_frame_delay_ms", "rs485", false, 5, 0, 1000, &ifd, err)) {
        return false;
    }
    cfg->rs485.baud = (uint32_t)baud;
    cfg->rs485.parity = (gw_parity_t)parity;
    cfg->rs485.stop_bits = (uint8_t)stop;
    cfg->rs485.response_timeout_ms = (uint16_t)tout;
    cfg->rs485.retries = (uint8_t)retries;
    cfg->rs485.inter_frame_delay_ms = (uint16_t)ifd;
    return true;
}

static bool parse_local_io(const cJSON *root, gw_config_t *cfg, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"di_debounce_ms"};
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, "local_io");
    static const cJSON empty = {.type = cJSON_Object};
    long db;
    if (o == NULL) {
        o = &empty;
    }
    if (!check_keys(o, "local_io", keys, 1, err) ||
        !get_int(o, "di_debounce_ms", "local_io", false, 20, 0, 1000, &db, err)) {
        return false;
    }
    cfg->local_io.di_debounce_ms = (uint16_t)db;
    return true;
}

static bool parse_tag(const cJSON *o, const char *path, gw_tag_cfg_t *t, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"name",   "area",   "address",  "type",     "word_order",
                                       "scale",  "offset", "deadband", "writable", "units"};
    int area, type, word;
    long addr;
    char p[96];
    if (!check_keys(o, path, keys, 10, err) ||
        !get_string(o, "name", path, true, "", t->name, sizeof(t->name), err) ||
        !get_enum(o, "area", path, true, 0, k_area_names, 4, &area, err) ||
        !get_int(o, "address", path, true, 0, 0, 65535, &addr, err) ||
        !get_enum(o, "type", path, true, 0, k_type_names, 6, &type, err) ||
        !get_enum(o, "word_order", path, false, GW_WORD_BIG, k_word_names, 2, &word, err) ||
        !get_double(o, "scale", path, 1.0, &t->scale, err) ||
        !get_double(o, "offset", path, 0.0, &t->offset, err) ||
        !get_double(o, "deadband", path, 0.0, &t->deadband, err) ||
        !get_bool(o, "writable", path, false, &t->writable, err) ||
        !get_string(o, "units", path, false, "", t->units, sizeof(t->units), err)) {
        return false;
    }
    t->area = (gw_area_t)area;
    t->type = (gw_type_t)type;
    t->word_order = (gw_word_order_t)word;
    t->address = (uint16_t)addr;

    if (t->name[0] == '\0') {
        join(p, sizeof(p), path, "name");
        return fail(err, p, "no puede estar vacío");
    }
    bool bit_area = t->area == GW_AREA_COIL || t->area == GW_AREA_DISCRETE;
    if (bit_area != (t->type == GW_TYPE_BOOL)) {
        join(p, sizeof(p), path, "type");
        return fail(err, p, bit_area ? "las áreas coil/discrete solo admiten type=bool"
                                     : "type=bool solo vale en las áreas coil y discrete");
    }
    if (t->writable && t->area != GW_AREA_COIL && t->area != GW_AREA_HOLDING) {
        join(p, sizeof(p), path, "writable");
        return fail(err, p, "solo las áreas coil y holding son escribibles");
    }
    if ((uint32_t)t->address + gw_tag_width(t) - 1u > 65535u) {
        join(p, sizeof(p), path, "address");
        return fail(err, p, "el tag excede la dirección 65535");
    }
    if (t->scale == 0.0) {
        join(p, sizeof(p), path, "scale");
        return fail(err, p, "no puede ser 0");
    }
    if (t->type == GW_TYPE_BOOL && (t->scale != 1.0 || t->offset != 0.0)) {
        join(p, sizeof(p), path, "scale");
        return fail(err, p, "scale/offset no se admiten en tags bool");
    }
    if (t->deadband < 0.0) {
        join(p, sizeof(p), path, "deadband");
        return fail(err, p, "no puede ser negativo");
    }
    return true;
}

static bool parse_device(const cJSON *o, const char *path, gw_config_t *cfg, gw_device_cfg_t *d,
                         gw_cfg_err_t *err)
{
    static const char *const keys[] = {"name", "unit_id", "poll_ms", "offline_after", "tags"};
    long unit, poll, off;
    char p[96];
    if (!check_keys(o, path, keys, 5, err) ||
        !get_string(o, "name", path, true, "", d->name, sizeof(d->name), err) ||
        !get_int(o, "unit_id", path, true, 0, 1, 247, &unit, err) ||
        !get_int(o, "poll_ms", path, false, 1000, 50, 3600000, &poll, err) ||
        !get_int(o, "offline_after", path, false, 3, 1, 1000, &off, err)) {
        return false;
    }
    join(p, sizeof(p), path, "name");
    if (!check_topic_id(d->name, p, false, err)) {
        return false;
    }
    d->unit_id = (uint8_t)unit;
    d->poll_ms = (uint32_t)poll;
    d->offline_after = (uint16_t)off;

    const cJSON *tags = cJSON_GetObjectItemCaseSensitive(o, "tags");
    join(p, sizeof(p), path, "tags");
    if (!cJSON_IsArray(tags)) {
        return fail(err, p, tags == NULL ? "campo obligatorio ausente" : "se esperaba un array");
    }
    int n = cJSON_GetArraySize(tags);
    if (n == 0) {
        return fail(err, p, "el dispositivo no tiene tags");
    }
    if (n > GW_MAX_TAGS_PER_DEVICE) {
        return fail(err, p, "%d tags; el máximo por dispositivo es %d", n, GW_MAX_TAGS_PER_DEVICE);
    }
    if (cfg->tag_count + n > GW_MAX_TAGS) {
        return fail(err, p, "se supera el máximo total de %d tags", GW_MAX_TAGS);
    }
    d->first_tag = cfg->tag_count;
    d->tag_count = 0;
    int i = 0;
    for (const cJSON *it = tags->child; it != NULL; it = it->next, i++) {
        char tp[96];
        snprintf(tp, sizeof(tp), "%s.tags[%d]", path, i);
        gw_tag_cfg_t *t = &cfg->tags[cfg->tag_count];
        if (!parse_tag(it, tp, t, err)) {
            return false;
        }
        for (uint16_t j = d->first_tag; j < cfg->tag_count; j++) {
            if (strcmp(cfg->tags[j].name, t->name) == 0) {
                join(p, sizeof(p), tp, "name");
                return fail(err, p, "nombre de tag duplicado '%s'", t->name);
            }
        }
        cfg->tag_count++;
        d->tag_count++;
    }
    return true;
}

static bool parse_devices(const cJSON *root, gw_config_t *cfg, gw_cfg_err_t *err)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (arr == NULL) {
        return true;
    }
    if (!cJSON_IsArray(arr)) {
        return fail(err, "devices", "se esperaba un array");
    }
    int n = cJSON_GetArraySize(arr);
    if (n > GW_MAX_DEVICES) {
        return fail(err, "devices", "%d dispositivos; el máximo es %d", n, GW_MAX_DEVICES);
    }
    int i = 0;
    for (const cJSON *it = arr->child; it != NULL; it = it->next, i++) {
        char path[32];
        snprintf(path, sizeof(path), "devices[%d]", i);
        gw_device_cfg_t *d = &cfg->devices[cfg->device_count];
        if (!parse_device(it, path, cfg, d, err)) {
            return false;
        }
        for (uint16_t j = 0; j < cfg->device_count; j++) {
            if (strcmp(cfg->devices[j].name, d->name) == 0) {
                char p[48];
                snprintf(p, sizeof(p), "%s.name", path);
                return fail(err, p, "nombre de dispositivo duplicado '%s'", d->name);
            }
        }
        cfg->device_count++;
    }
    return true;
}

bool gw_config_parse(const char *json, size_t len, gw_config_t *cfg, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"network", "ntp",   "mqtt",     "sparkplug",
                                       "rs485",   "local_io", "devices"};
    memset(cfg, 0, sizeof(*cfg));
    err->msg[0] = '\0';

    cJSON_Hooks hooks = {.malloc_fn = tracked_alloc, .free_fn = s_release};
    cJSON_InitHooks(&hooks);
    s_alloc_failed = false;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, len, &end, false);
    if (root == NULL && s_alloc_failed) {
        fail(err, "$", "memoria insuficiente para analizar %zu bytes de JSON", len);
        return false;
    }
    if (root == NULL) {
        const char *at = cJSON_GetErrorPtr();
        return fail_at(err, json, (at != NULL && at >= json && at <= json + len) ? at : json,
                       "sintaxis no válida");
    }
    while (end < json + len && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        end++;
    }
    if (end < json + len && *end != '\0') {
        cJSON_Delete(root);
        return fail_at(err, json, end, "contenido después del documento");
    }
    bool ok = check_keys(root, "", keys, 7, err) && parse_network(root, cfg, err) &&
              parse_ntp(root, cfg, err) && parse_mqtt(root, cfg, err) &&
              parse_sparkplug(root, cfg, err) && parse_rs485(root, cfg, err) &&
              parse_local_io(root, cfg, err) && parse_devices(root, cfg, err);
    cJSON_Delete(root);

    if (ok && cfg->mqtt.client_id[0] == '\0') {
        strcpy(cfg->mqtt.client_id, cfg->sparkplug.edge_node_id);
    }
    if (!ok) {
        memset(cfg, 0, sizeof(*cfg));
    }
    return ok;
}

int gw_config_find_device(const gw_config_t *cfg, const char *name, size_t len)
{
    for (uint16_t i = 0; i < cfg->device_count; i++) {
        if (strlen(cfg->devices[i].name) == len && memcmp(cfg->devices[i].name, name, len) == 0) {
            return i;
        }
    }
    return -1;
}

int gw_config_find_tag(const gw_config_t *cfg, uint16_t dev, const char *name)
{
    const gw_device_cfg_t *d = &cfg->devices[dev];
    for (uint16_t i = d->first_tag; i < d->first_tag + d->tag_count; i++) {
        if (strcmp(cfg->tags[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

uint16_t gw_config_tag_device(const gw_config_t *cfg, uint16_t tag)
{
    for (uint16_t i = 0; i < cfg->device_count; i++) {
        const gw_device_cfg_t *d = &cfg->devices[i];
        if (tag >= d->first_tag && tag < d->first_tag + d->tag_count) {
            return i;
        }
    }
    return 0;
}

uint16_t gw_tag_width(const gw_tag_cfg_t *tag)
{
    switch (tag->type) {
    case GW_TYPE_UINT32:
    case GW_TYPE_INT32:
    case GW_TYPE_FLOAT32:
        return 2;
    default:
        return 1;
    }
}
