#include "gw_config.h"
#include "gw_conv.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <float.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "gw_config_priv.h"
#include "gw_port.h"
#include "gw_sfspill.h"
#include "gw_utf8.h"
#include "gw_wpa.h"

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

void gw_config_json_hooks(void)
{
    cJSON_Hooks hooks = {.malloc_fn = tracked_alloc, .free_fn = s_release};
    cJSON_InitHooks(&hooks);
    s_alloc_failed = false;
}

bool gw_config_json_alloc_failed(void)
{
    return s_alloc_failed;
}

void *gw_config_alloc(size_t size)
{
    return s_alloc(size);
}

void gw_config_release(void *ptr)
{
    if (ptr != NULL) {
        s_release(ptr);
    }
}

/* ---------- error reporting ---------- */

static bool fail(gw_cfg_err_t *err, const char *path, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static bool fail(gw_cfg_err_t *err, const char *path, const char *fmt, ...)
{
    snprintf(err->path, sizeof(err->path), "%s", path);
    err->line = err->column = 0;
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

/* ---------- issue #144 item 3: the "since" schema of each key and enum value ----------
 * Keys and enum values that came after schema 1: {path ("[]" for an index), enum value or NULL
 * for the key, since}. Add each new key or value here with GW_CONFIG_SCHEMA + 1 and raise
 * GW_CONFIG_SCHEMA. The last entry only ends the table. */
static const gw_since_t k_since[] = {
    /* schema 2: the keys of #195 (no migration before v1.0: the replaced keys were dropped) */
    {"gateway", NULL, 2},                          /* #206 */
    {"gateway.name", NULL, 2},
    {"gateway.location", NULL, 2},
    {"gateway.comment", NULL, 2},
    {"ntp.servers", NULL, 2},                      /* #201 (replaces ntp.server) */
    {"local_io.di", NULL, 2},                      /* #211 (replaces local_io.di_debounce_ms) */
    {"local_io.di[].invert", NULL, 2},
    {"local_io.di[].debounce_ms", NULL, 2},
    {"local_io.do_pulse_ms", NULL, 2},             /* #210 */
    {"local_io.fast_counters", NULL, 2},           /* #218 */
    {"local_io.fast_counters[].di", NULL, 2},
    {"local_io.fast_counters[].glitch_ns", NULL, 2},
    {"local_io.run_hours", NULL, 2},               /* #217 */
    {"modbus_tcp.local_unit_id", NULL, 2},         /* #216 */
    {"modbus_tcp.local_writes", NULL, 2},          /* #216 */
    {"ota.mqtt", NULL, 2},                         /* #223 (replaces ota.enabled) */
    {"devices[].enabled", NULL, 2},                /* #214 */
    {"devices[].write_multiple", NULL, 2},         /* #197 */
    {"devices[].tags[].write_min", NULL, 2},       /* #196 */
    {"devices[].tags[].write_max", NULL, 2},       /* #196 */
    {"devices[].tags[].bit_offset", NULL, 2},      /* #202 (replaces bit) */
    {"devices[].tags[].bit_width", NULL, 2},       /* #202 */
    {"devices[].type", NULL, 2},                   /* #213: the name of a device type */
    {"device_types", NULL, 2},                     /* #213 */
    {"device_types[].name", NULL, 2},
    {"device_types[].tags", NULL, 2},
    {"device_types[].tags[].bit_offset", NULL, 2},
    {"device_types[].tags[].bit_width", NULL, 2},
    {"", NULL, 0},
};
#define SINCE_COUNT (sizeof(k_since) / sizeof(k_since[0]) - 1)

void gw_config_schema_init(gw_schema_report_t *rep)
{
    gw_schema_init(rep, k_since, SINCE_COUNT);
}

const gw_since_t *gw_config_since_table(size_t *n)
{
    *n = SINCE_COUNT;
    return k_since;
}

/* The report of the parse in progress (under the JSON lock), NULL: none asked for. */
static gw_schema_report_t *s_rep;

static void note(const char *path, const char *value)
{
    if (s_rep != NULL) {
        gw_schema_note(s_rep, path, value);
    }
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
    err->path[0] = '\0';
    err->line = line;
    err->column = col;
    snprintf(err->msg, sizeof(err->msg), "JSON syntax error at line %u, column %u: %s", line,
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
        return fail(err, path[0] != '\0' ? path : "$", "expected a JSON object");
    }
    for (const cJSON *it = obj->child; it != NULL; it = it->next) {
        char p[96];
        join(p, sizeof(p), path, it->string);
        bool known = false;
        for (size_t i = 0; i < n && !known; i++) {
            known = strcmp(it->string, allowed[i]) == 0;
        }
        if (!known) {
            return fail(err, p, "unknown key");
        }
        note(p, NULL);
        for (const cJSON *prev = obj->child; prev != it; prev = prev->next) {
            if (strcmp(prev->string, it->string) == 0) {
                return fail(err, p, "duplicate key");
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
        return fail(err, p, "expected true or false");
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
            return fail(err, p, "missing required field");
        }
        *out = def;
        return true;
    }
    if (!cJSON_IsNumber(v) || v->valuedouble != floor(v->valuedouble)) {
        return fail(err, p, "expected an integer");
    }
    if (v->valuedouble < (double)min || v->valuedouble > (double)max) {
        return fail(err, p, "value %.0f out of range [%ld, %ld]", v->valuedouble, min, max);
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
        return fail(err, p, "expected a number");
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
            return fail(err, p, "missing required field");
        }
    } else if (!cJSON_IsString(v)) {
        return fail(err, p, "expected a string");
    } else {
        s = v->valuestring;
    }
    size_t len = strlen(s);
    if (len >= cap) {
        return fail(err, p, "string too long (%zu characters, maximum %zu)", len, cap - 1);
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
            return fail(err, p, "missing required field");
        }
        *out = def;
        return true;
    }
    if (cJSON_IsString(v)) {
        for (size_t i = 0; i < n; i++) {
            if (strcmp(v->valuestring, names[i]) == 0) {
                *out = (int)i;
                note(p, names[i]);
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
    return fail(err, p, "invalid value (expected %s)", allowed);
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
            return fail(err, p, "IPv4 address required with dhcp=false");
        }
        memset(out, 0, 4);
        return true;
    }
    if (!parse_ipv4(buf, out)) {
        return fail(err, p, "'%s' is not a valid IPv4 address", buf);
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

bool gw_parse_ipv6(const char *s, uint8_t out[16])
{
    uint16_t head[8], tail[8];
    unsigned nh = 0, nt = 0;
    bool gap = false;   /* "::" seen; later groups go to tail[] */
    const char *p = s;
    if (p[0] == ':') {
        if (p[1] != ':') {
            return false;
        }
        gap = true;
        p += 2;
    }
    while (*p != '\0') {
        if (nh + nt >= 8) {
            return false;
        }
        const char *colon = strchr(p, ':');
        const char *dot = strchr(p, '.');
        if (dot != NULL && (colon == NULL || dot < colon)) {
            /* trailing dotted quad (::ffff:192.0.2.1): two groups, must be last */
            uint8_t v4[4];
            if (nh + nt > 6 || !parse_ipv4(p, v4)) {
                return false;
            }
            uint16_t *g = gap ? tail : head;
            unsigned *n = gap ? &nt : &nh;
            g[(*n)++] = (uint16_t)(v4[0] << 8 | v4[1]);
            g[(*n)++] = (uint16_t)(v4[2] << 8 | v4[3]);
            p += strlen(p);
            break;
        }
        unsigned v = 0, digits = 0;
        for (int d; (d = hex_digit(*p)) >= 0; p++) {
            if (++digits > 4) {
                return false;
            }
            v = v * 16u + (unsigned)d;
        }
        if (digits == 0) {
            return false;
        }
        if (gap) {
            tail[nt++] = (uint16_t)v;
        } else {
            head[nh++] = (uint16_t)v;
        }
        if (*p == '\0') {
            break;
        }
        if (*p++ != ':') {
            return false;
        }
        if (*p == ':') {
            if (gap) {
                return false;   /* a second "::" */
            }
            gap = true;
            p++;
        } else if (*p == '\0') {
            return false;       /* trailing single ':' */
        }
    }
    if (gap ? nh + nt > 7 : nh != 8) {
        return false;
    }
    uint16_t g[8] = {0};
    for (unsigned i = 0; i < nh; i++) {
        g[i] = head[i];
    }
    for (unsigned i = 0; i < nt; i++) {
        g[8 - nt + i] = tail[i];
    }
    for (unsigned i = 0; i < 8; i++) {
        out[2 * i] = (uint8_t)(g[i] >> 8);
        out[2 * i + 1] = (uint8_t)g[i];
    }
    return true;
}

static bool get_ipv6(const cJSON *obj, const char *key, const char *path, uint8_t out[16],
                     bool *present, gw_cfg_err_t *err)
{
    char p[96];
    join(p, sizeof(p), path, key);
    char buf[48];
    if (!get_string(obj, key, path, false, "", buf, sizeof(buf), err)) {
        return false;
    }
    *present = buf[0] != '\0';
    if (!*present) {
        memset(out, 0, 16);
        return true;
    }
    if (!gw_parse_ipv6(buf, out)) {
        return fail(err, p, "'%s' is not a valid IPv6 address", buf);
    }
    return true;
}

/* Issue #162: text that goes into a Sparkplug string (a metric name, a template name, the
 * engineering units) must be well-formed UTF-8: a protobuf decoder may refuse it otherwise. */
static bool check_utf8(const char *s, const char *path, gw_cfg_err_t *err)
{
    size_t n = strlen(s);
    for (size_t i = 0; i < n;) {
        size_t k = gw_utf8_seq(s + i, n - i);
        if (k == 0) {
            return fail(err, path, "not valid UTF-8 (byte 0x%02x at offset %u)",
                        (unsigned char)s[i], (unsigned)i);
        }
        i += k;
    }
    return true;
}

/* Sparkplug identifiers become MQTT topic levels: no '/', '+', '#', no control chars, and
 * well-formed UTF-8 (issue #162: tck-id-topic-structure-namespace-valid-group-id,
 * -valid-edge-node-id, -valid-device-id; MQTT 3.1.1 section 1.5.3: a broker may close the
 * connection for a topic that is not UTF-8 or that has a control character, also U+0080-U+009F). */
static bool check_topic_id(const char *s, const char *path, bool allow_empty, gw_cfg_err_t *err)
{
    if (s[0] == '\0') {
        return allow_empty ? true : fail(err, path, "cannot be empty");
    }
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        if (*c == '/' || *c == '+' || *c == '#' || *c < 0x20 || *c == 0x7f) {
            return fail(err, path,
                        "character not allowed '%c' (forbidden: / + # and control characters)",
                        *c >= 0x20 ? *c : '?');
        }
    }
    if (!check_utf8(s, path, err)) {
        return false;
    }
    size_t n = strlen(s);
    for (size_t i = 0; i < n;) {
        size_t k = gw_utf8_seq(s + i, n - i);
        unsigned long cp = gw_utf8_code(s + i, k);
        if (cp >= 0x80 && cp <= 0x9f) {
            return fail(err, path, "control character U+%04lX not allowed", cp);
        }
        i += k;
    }
    return true;
}

/* Issue #162: Sparkplug IDs and metric names SHOULD NOT differ only in case
 * (tck-id-case-sensitivity-sparkplug-ids, tck-id-case-sensitivity-metric-names): a host may
 * store them in a case-insensitive system. The comparison folds ASCII letters only. */
static bool same_but_case(const char *a, const char *b)
{
    return strcmp(a, b) != 0 && strcasecmp(a, b) == 0;
}

/* Issue #162: true if metric name @p a is a folder of metric name @p b ("A" and "A/B"):
 * the slashes of a metric name make folders, and "a defined folder should always remain a
 * folder" (Sparkplug B 3.0, chapter 6, Metric: name). Without case, as the comparison above. */
static bool folder_of(const char *a, const char *b)
{
    size_t n = strlen(a);
    return strncasecmp(a, b, n) == 0 && b[n] == '/';
}

/* ---------- sections ---------- */

static const char *const k_area_names[] = {"coil", "discrete", "holding", "input"};
static const char *const k_type_names[] = {"bool",  "uint16", "int16",   "uint32", "int32",
                                           "float32", "uint64", "int64", "float64", "string",
                                           "bytes"};
#define TYPE_COUNT (sizeof(k_type_names) / sizeof(k_type_names[0]))
/* "big"/"little" are the MVP names of ABCD/CDAB (MB-5); the four letter forms come with MB-6. */
static const char *const k_word_names[] = {"big", "little", "ABCD", "CDAB", "BADC", "DCBA"};
static const gw_word_order_t k_word_values[] = {GW_WORD_BIG,    GW_WORD_LITTLE, GW_WORD_BIG,
                                                GW_WORD_LITTLE, GW_WORD_BADC,   GW_WORD_DCBA};
static const char *const k_parity_names[] = {"none", "even", "odd"};
static const char *const k_naming_names[] = {"sparkplug", "umh"};   /* gw_naming_t (#169) */

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

static bool parse_ipv6_section(const cJSON *net, gw_config_t *cfg, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"enabled", "slaac", "address", "dns"};
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(net, "ipv6");
    static const cJSON empty = {.type = cJSON_Object};
    if (o == NULL) {
        o = &empty;
    }
    if (!check_keys(o, "network.ipv6", keys, 4, err) ||
        !get_bool(o, "enabled", "network.ipv6", false, &cfg->network.ipv6.enabled, err) ||
        !get_bool(o, "slaac", "network.ipv6", true, &cfg->network.ipv6.slaac, err) ||
        !get_ipv6(o, "address", "network.ipv6", cfg->network.ipv6.address,
                  &cfg->network.ipv6.has_address, err) ||
        !get_ipv6(o, "dns", "network.ipv6", cfg->network.ipv6.dns, &cfg->network.ipv6.has_dns,
                  err)) {
        return false;
    }
    const uint8_t *a = cfg->network.ipv6.address;
    if (cfg->network.ipv6.has_address) {
        static const uint8_t zero[16];
        if (a[0] == 0xff || (a[0] == 0xfe && (a[1] & 0xc0) == 0x80) || memcmp(a, zero, 15) == 0) {
            return fail(err, "network.ipv6.address",
                        "expected a global unicast or unique local address (not multicast, "
                        "link-local, :: or ::1)");
        }
    }
    return true;
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
        return fail(err, "network.wifi.ssid", "required with enabled=true");
    }
    if (!check_wifi_password(w->password)) {
        return fail(err, "network.wifi.password",
                    "expected 8 to 63 characters (WPA2), 64 hexadecimal digits or empty "
                    "(open network)");
    }
    return true;
}

void gw_hostname_from(const char *src, char *out, size_t cap)
{
    size_t n = 0;
    size_t max = cap - 1 < 63 ? cap - 1 : 63;
    for (const unsigned char *c = (const unsigned char *)src; *c && n < max; c++) {
        char ch = (char)((*c >= 'A' && *c <= 'Z') ? *c - 'A' + 'a' : *c);
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
        if (ok) {
            out[n++] = ch;
        } else if (n > 0 && out[n - 1] != '-') {
            out[n++] = '-';
        }
    }
    while (n > 0 && out[n - 1] == '-') {
        n--;
    }
    out[n] = '\0';
    if (n == 0) {
        snprintf(out, cap, "gateway");
    }
}

/* RFC 1123 label: letters, digits and '-', not at either end. */
static bool check_hostname(const char *h, gw_cfg_err_t *err)
{
    size_t n = strlen(h);
    if (n == 0) {
        return true;
    }
    for (size_t i = 0; i < n; i++) {
        char c = h[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  (c == '-' && i > 0 && i + 1 < n);
        if (!ok) {
            return fail(err, "network.hostname",
                        "expected letters, digits and '-' (not at the start or the end), no "
                        "dots: it is a host name, not an FQDN");
        }
    }
    return true;
}

static bool parse_network(const cJSON *root, gw_config_t *cfg, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"dhcp", "ip",   "netmask", "gateway", "dns",
                                       "ipv6", "wifi", "hostname", "mdns"};
    static const cJSON empty = {.type = cJSON_Object};
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, "network");
    if (o == NULL) {
        o = &empty;
    }
    return check_keys(o, "network", keys, 9, err) &&
           get_string(o, "hostname", "network", false, "", cfg->network.hostname,
                      sizeof(cfg->network.hostname), err) &&
           check_hostname(cfg->network.hostname, err) &&
           get_bool(o, "mdns", "network", true, &cfg->network.mdns, err) &&
           parse_ipv4_keys(o, "network", &cfg->network.dhcp, cfg->network.ip,
                           cfg->network.netmask, cfg->network.gateway, cfg->network.dns, err) &&
           parse_ipv6_section(o, cfg, err) && parse_wifi_section(o, cfg, err);
}

/* #201: "ntp": {"servers": ["pool.ntp.org", ...]}, 1 to GW_MAX_NTP_SERVERS names or addresses,
 * tried in order by the SNTP client. Before v1.0 the old key "server" is gone, with no migration. */
static bool parse_ntp(const cJSON *root, gw_config_t *cfg, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"servers"};
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, "ntp");
    const cJSON *list = o != NULL ? cJSON_GetObjectItemCaseSensitive(o, "servers") : NULL;
    if (o != NULL && !check_keys(o, "ntp", keys, 1, err)) {
        return false;
    }
    if (list == NULL) {
        strcpy(cfg->ntp.servers[0], "pool.ntp.org");
        cfg->ntp.server_count = 1;
        return true;
    }
    if (!cJSON_IsArray(list)) {
        return fail(err, "ntp.servers", "expected an array of 1 to %d names or addresses",
                    GW_MAX_NTP_SERVERS);
    }
    int n = cJSON_GetArraySize(list);
    if (n < 1 || n > GW_MAX_NTP_SERVERS) {
        return fail(err, "ntp.servers", "%d servers; expected 1 to %d", n, GW_MAX_NTP_SERVERS);
    }
    int i = 0;
    for (const cJSON *it = list->child; it != NULL; it = it->next, i++) {
        char p[24];
        snprintf(p, sizeof(p), "ntp.servers[%d]", i);
        if (!cJSON_IsString(it)) {
            return fail(err, p, "expected a string (a name, an IPv4 or an IPv6 address)");
        }
        size_t len = strlen(it->valuestring);
        if (len == 0) {
            return fail(err, p, "cannot be empty");
        }
        if (len >= GW_HOST_SIZE) {
            return fail(err, p, "%u bytes; the maximum is %d", (unsigned)len, GW_HOST_SIZE - 1);
        }
        memcpy(cfg->ntp.servers[i], it->valuestring, len + 1);
    }
    cfg->ntp.server_count = (uint8_t)n;
    return true;
}

bool gw_config_file_name_ok(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n >= GW_FILE_SIZE || name[0] == '.' || strncmp(name, "config.json", 11) == 0) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

static bool get_file_name(const cJSON *o, const char *key, const char *path, char *out,
                          gw_cfg_err_t *err)
{
    if (!get_string(o, key, path, false, "", out, GW_FILE_SIZE, err)) {
        return false;
    }
    if (out[0] != '\0' && !gw_config_file_name_ok(out)) {
        char p[96];
        join(p, sizeof(p), path, key);
        return fail(err, p, "'%s' is not a valid file name (1-%d characters A-Z a-z 0-9 . _ "
                    "-, no '/', not config.json)", out, GW_FILE_SIZE - 1);
    }
    return true;
}

/* MQTT-4 / MQTT-5 */
static bool parse_tls(const cJSON *mqtt, gw_tls_cfg_t *t, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"enabled", "ca_file", "server_name", "cert_file",
                                       "key_file"};
    static const cJSON empty = {.type = cJSON_Object};
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(mqtt, "tls");
    if (o == NULL) {
        o = &empty;
    }
    if (!check_keys(o, "mqtt.tls", keys, 5, err) ||
        !get_bool(o, "enabled", "mqtt.tls", false, &t->enabled, err) ||
        !get_file_name(o, "ca_file", "mqtt.tls", t->ca_file, err) ||
        !get_string(o, "server_name", "mqtt.tls", false, "", t->server_name,
                    sizeof(t->server_name), err) ||
        !get_file_name(o, "cert_file", "mqtt.tls", t->cert_file, err) ||
        !get_file_name(o, "key_file", "mqtt.tls", t->key_file, err)) {
        return false;
    }
    if ((t->cert_file[0] == '\0') != (t->key_file[0] == '\0')) {
        return fail(err, t->cert_file[0] ? "mqtt.tls.key_file" : "mqtt.tls.cert_file",
                    "set cert_file and key_file together (mutual TLS)");
    }
    return true;
}

/* MQTT-6: [{"host": "...", "port": n}, ...], tried in order after mqtt.host. */
static bool parse_backup_brokers(const cJSON *mqtt, gw_config_t *cfg, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"host", "port"};
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(mqtt, "backup_brokers");
    if (arr == NULL) {
        return true;
    }
    if (!cJSON_IsArray(arr)) {
        return fail(err, "mqtt.backup_brokers", "expected an array");
    }
    int n = cJSON_GetArraySize(arr);
    if (n > GW_MAX_BACKUP_BROKERS) {
        return fail(err, "mqtt.backup_brokers", "%d brokers; the maximum is %d", n,
                    GW_MAX_BACKUP_BROKERS);
    }
    int i = 0;
    for (const cJSON *it = arr->child; it != NULL; it = it->next, i++) {
        char path[40];
        snprintf(path, sizeof(path), "mqtt.backup_brokers[%d]", i);
        long port;
        if (!check_keys(it, path, keys, 2, err) ||
            !get_string(it, "host", path, true, "", cfg->mqtt.backup[i].host,
                        sizeof(cfg->mqtt.backup[i].host), err) ||
            !get_int(it, "port", path, false, cfg->mqtt.port, 1, 65535, &port, err)) {
            return false;
        }
        if (cfg->mqtt.backup[i].host[0] == '\0') {
            char p[56];
            snprintf(p, sizeof(p), "%s.host", path);
            return fail(err, p, "cannot be empty");
        }
        cfg->mqtt.backup[i].port = (uint16_t)port;
    }
    cfg->mqtt.backup_count = (uint8_t)n;
    return true;
}

static bool parse_mqtt(const cJSON *root, gw_config_t *cfg, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"host", "port", "client_id", "username", "password",
                                       "keepalive_s", "tls", "backup_brokers", "failover_after",
                                       "primary_return_s", "protocol"};
    static const char *const protocols[] = {"3.1.1", "5"};
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, "mqtt");
    if (o == NULL) {
        return fail(err, "mqtt", "missing required section");
    }
    long port, ka, failover, back;
    int proto;
    if (!check_keys(o, "mqtt", keys, 11, err) || !parse_tls(o, &cfg->mqtt.tls, err) ||
        !get_string(o, "host", "mqtt", true, "", cfg->mqtt.host, sizeof(cfg->mqtt.host), err) ||
        !get_int(o, "port", "mqtt", false, cfg->mqtt.tls.enabled ? 8883 : 1883, 1, 65535, &port,
                 err) ||
        !get_string(o, "client_id", "mqtt", false, "", cfg->mqtt.client_id,
                    sizeof(cfg->mqtt.client_id), err) ||
        !get_string(o, "username", "mqtt", false, "", cfg->mqtt.username,
                    sizeof(cfg->mqtt.username), err) ||
        !get_string(o, "password", "mqtt", false, "", cfg->mqtt.password,
                    sizeof(cfg->mqtt.password), err) ||
        !get_int(o, "keepalive_s", "mqtt", false, 30, 5, 3600, &ka, err) ||
        !get_int(o, "failover_after", "mqtt", false, 3, 1, 20, &failover, err) ||
        !get_int(o, "primary_return_s", "mqtt", false, 0, 0, 86400, &back, err) ||
        !get_enum(o, "protocol", "mqtt", false, GW_MQTT_V311, protocols, 2, &proto, err)) {
        return false;
    }
    if (cfg->mqtt.host[0] == '\0') {
        return fail(err, "mqtt.host", "cannot be empty");
    }
    cfg->mqtt.port = (uint16_t)port;
    cfg->mqtt.keepalive_s = (uint16_t)ka;
    cfg->mqtt.failover_after = (uint8_t)failover;
    cfg->mqtt.primary_return_s = (uint32_t)back;
    cfg->mqtt.protocol = (gw_mqtt_protocol_t)proto;
    return parse_backup_brokers(o, cfg, err);
}

/* SPB-14: true/false, or {"enabled": bool, "sd": {"enabled": bool, "max_kb": n}}. */
static bool parse_store_forward(const cJSON *sp, gw_config_t *cfg, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"enabled", "sd"};
    static const char *const sd_keys[] = {"enabled", "max_kb"};
    static const cJSON empty = {.type = cJSON_Object};
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(sp, "store_forward");
    cfg->sparkplug.sf_sd_max_kb = GW_SFSPILL_DEFAULT_KB;
    if (o == NULL || cJSON_IsBool(o)) {
        cfg->sparkplug.store_forward = cJSON_IsTrue(o);
        return true;
    }
    if (!cJSON_IsObject(o)) {
        return fail(err, "sparkplug.store_forward", "expected true, false or an object");
    }
    const cJSON *sd = cJSON_GetObjectItemCaseSensitive(o, "sd");
    long max_kb;
    if (!check_keys(o, "sparkplug.store_forward", keys, 2, err) ||
        !get_bool(o, "enabled", "sparkplug.store_forward", true, &cfg->sparkplug.store_forward,
                  err) ||
        !check_keys(sd != NULL ? sd : &empty, "sparkplug.store_forward.sd", sd_keys, 2, err) ||
        !get_bool(sd != NULL ? sd : &empty, "enabled", "sparkplug.store_forward.sd", false,
                  &cfg->sparkplug.sf_sd, err) ||
        !get_int(sd != NULL ? sd : &empty, "max_kb", "sparkplug.store_forward.sd", false,
                 GW_SFSPILL_DEFAULT_KB, GW_SFSPILL_MIN_KB, GW_SFSPILL_MAX_KB, &max_kb, err)) {
        return false;
    }
    cfg->sparkplug.sf_sd_max_kb = (uint32_t)max_kb;
    if (cfg->sparkplug.sf_sd && !cfg->sparkplug.store_forward) {
        return fail(err, "sparkplug.store_forward.sd.enabled",
                    "the card only extends the buffer: it needs store_forward.enabled");
    }
    return true;
}

/* #172: sparkplug.device_prefix is a list of location levels with ':' between them: a topic ID
 * (check_topic_id), and no empty level (no ':' at the start or the end, no "::"). The checks of
 * the characters of each level for UNS hosts are in gw_config_lint (#169). */
static bool check_device_prefix(const char *s, gw_cfg_err_t *err)
{
    static const char *const path = "sparkplug.device_prefix";
    if (!check_topic_id(s, path, true, err)) {
        return false;
    }
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++) {
        if (s[i] == ':' && (i == 0 || i + 1 == n || s[i + 1] == ':')) {
            return fail(err, path, "empty level at byte %u: put one ':' between two levels, and "
                        "no ':' at the start or the end", (unsigned)i);
        }
    }
    return true;
}

static bool parse_sparkplug(const cJSON *root, gw_config_t *cfg, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"group_id", "edge_node_id", "primary_host_id",
                                       "store_forward", "diagnostics_s", "device_prefix",
                                       "naming"};
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, "sparkplug");
    if (o == NULL) {
        return fail(err, "sparkplug", "missing required section");
    }
    long diag;
    int naming;
    if (!check_keys(o, "sparkplug", keys, sizeof(keys) / sizeof(keys[0]), err) ||
        !get_int(o, "diagnostics_s", "sparkplug", false, 60, 0, GW_DIAG_MAX_S, &diag, err) ||
        !get_enum(o, "naming", "sparkplug", false, GW_NAMING_SPARKPLUG, k_naming_names, 2,
                  &naming, err)) {
        return false;
    }
    cfg->sparkplug.naming = (gw_naming_t)naming;
    if (diag != 0 && diag < GW_DIAG_MIN_S) {
        return fail(err, "sparkplug.diagnostics_s", "0 (only in the NBIRTH) or %d-%d s",
                    GW_DIAG_MIN_S, GW_DIAG_MAX_S);
    }
    cfg->sparkplug.diagnostics_s = (uint32_t)diag;
    return parse_store_forward(o, cfg, err) &&
           get_string(o, "group_id", "sparkplug", true, "", cfg->sparkplug.group_id,
                      sizeof(cfg->sparkplug.group_id), err) &&
           get_string(o, "edge_node_id", "sparkplug", true, "", cfg->sparkplug.edge_node_id,
                      sizeof(cfg->sparkplug.edge_node_id), err) &&
           get_string(o, "primary_host_id", "sparkplug", false, "",
                      cfg->sparkplug.primary_host_id, sizeof(cfg->sparkplug.primary_host_id),
                      err) &&
           get_string(o, "device_prefix", "sparkplug", false, "", cfg->sparkplug.device_prefix,
                      sizeof(cfg->sparkplug.device_prefix), err) &&
           check_topic_id(cfg->sparkplug.group_id, "sparkplug.group_id", false, err) &&
           check_topic_id(cfg->sparkplug.edge_node_id, "sparkplug.edge_node_id", false, err) &&
           check_topic_id(cfg->sparkplug.primary_host_id, "sparkplug.primary_host_id", true, err) &&
           check_device_prefix(cfg->sparkplug.device_prefix, err);
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

/* IO-6: local_io.buzzer {"alarms": [...], "alarm_after_s": n} */
static bool parse_buzzer(const cJSON *local_io, gw_config_t *cfg, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"alarms", "alarm_after_s"};
    static const char *const names[] = {"device_offline", "mqtt_down", "config_error"};
    static const cJSON empty = {.type = cJSON_Object};
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(local_io, "buzzer");
    long after;
    if (o == NULL) {
        o = &empty;
    }
    if (!check_keys(o, "local_io.buzzer", keys, 2, err) ||
        !get_int(o, "alarm_after_s", "local_io.buzzer", false, 30, 0, GW_ALARM_AFTER_MAX_S, &after,
                 err)) {
        return false;
    }
    cfg->local_io.buzzer.alarm_after_s = (uint16_t)after;
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(o, "alarms");
    if (list == NULL) {
        return true;
    }
    if (!cJSON_IsArray(list)) {
        return fail(err, "local_io.buzzer.alarms", "expected an array");
    }
    int i = 0;
    for (const cJSON *it = list->child; it != NULL; it = it->next, i++) {
        char p[40];
        snprintf(p, sizeof(p), "local_io.buzzer.alarms[%d]", i);
        int a = -1;
        for (int k = 0; cJSON_IsString(it) && k < 3; k++) {
            if (strcmp(it->valuestring, names[k]) == 0) {
                a = k + 1;   /* gw_alarm_t */
            }
        }
        if (a < 0) {
            return fail(err, p, "invalid value (expected device_offline|mqtt_down|config_error)");
        }
        if (cfg->local_io.buzzer.alarms & GW_ALARM_BIT(a)) {
            return fail(err, p, "duplicate alarm");
        }
        cfg->local_io.buzzer.alarms |= (uint8_t)GW_ALARM_BIT(a);
    }
    return true;
}

/* IO-7: local_io.rules [{"di": 1-8, "do": 1-8, "mode": "follow|invert|toggle"}], one per DO.
 * Called once do_failsafe and do_pulse_ms are parsed: a rule's DO cannot have the safe state
 * "on", and cannot be a pulse DO (#210). */
static bool parse_rules(const cJSON *local_io, gw_config_t *cfg, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"di", "do", "mode"};
    static const char *const modes[] = {"follow", "invert", "toggle"};   /* gw_rule_mode_t */
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(local_io, "rules");
    if (list == NULL) {
        return true;
    }
    if (!cJSON_IsArray(list)) {
        return fail(err, "local_io.rules", "expected an array");
    }
    int n = cJSON_GetArraySize(list);
    if (n > GW_LOCAL_IO_CHANNELS) {
        return fail(err, "local_io.rules", "%d rules; the maximum is %d (one per DO)", n,
                    GW_LOCAL_IO_CHANNELS);
    }
    int i = 0;
    for (const cJSON *it = list->child; it != NULL; it = it->next, i++) {
        char p[40];
        snprintf(p, sizeof(p), "local_io.rules[%d]", i);
        long di, dout;
        int mode;
        if (!check_keys(it, p, keys, 3, err) ||
            !get_int(it, "di", p, true, 0, 1, GW_LOCAL_IO_CHANNELS, &di, err) ||
            !get_int(it, "do", p, true, 0, 1, GW_LOCAL_IO_CHANNELS, &dout, err) ||
            !get_enum(it, "mode", p, false, GW_RULE_FOLLOW, modes, 3, &mode, err)) {
            return false;
        }
        for (int k = 0; k < i; k++) {
            if (cfg->local_io.rules[k].dout == dout - 1) {
                char q[48];
                snprintf(q, sizeof(q), "%s.do", p);
                return fail(err, q, "DO/%ld already has a rule: local_io.rules[%d]", dout, k);
            }
        }
        if (cfg->local_io.do_pulse_ms[dout - 1] != 0) {
            /* #210: a pulse DO goes off by itself: a rule cannot hold it */
            return fail(err, p, "DO/%ld is a pulse DO (local_io.do_pulse_ms[%ld]): a local rule "
                                "cannot drive it", dout, dout - 1);
        }
        if (cfg->local_io.do_failsafe[dout - 1] == GW_DO_SAFE_ON) {
            char q[48];
            snprintf(q, sizeof(q), "local_io.do_failsafe[%ld]", dout - 1);
            return fail(err, q, "DO/%ld has a rule (local_io.rules[%d]): the safe state does not "
                                "apply to it (use off or hold)", dout, i);
        }
        cfg->local_io.rules[i].di = (uint8_t)(di - 1);
        cfg->local_io.rules[i].dout = (uint8_t)(dout - 1);
        cfg->local_io.rules[i].mode = (uint8_t)mode;
    }
    cfg->local_io.rule_count = (uint8_t)n;
    return true;
}

/* #211: local_io.di [{"invert": false, "debounce_ms": 20}, ...], DI/1 first; a missing entry
 * or key takes the default. */
static bool parse_di(const cJSON *local_io, gw_config_t *cfg, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"invert", "debounce_ms"};
    for (int i = 0; i < GW_LOCAL_IO_CHANNELS; i++) {
        cfg->local_io.di_debounce_ms[i] = GW_DI_DEBOUNCE_DEF_MS;
    }
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(local_io, "di");
    if (list == NULL) {
        return true;
    }
    if (!cJSON_IsArray(list)) {
        return fail(err, "local_io.di", "expected an array (DI/1 first)");
    }
    int n = cJSON_GetArraySize(list);
    if (n > GW_LOCAL_IO_CHANNELS) {
        return fail(err, "local_io.di", "%d entries; there are %d inputs", n, GW_LOCAL_IO_CHANNELS);
    }
    int i = 0;
    for (const cJSON *it = list->child; it != NULL; it = it->next, i++) {
        char p[40];
        snprintf(p, sizeof(p), "local_io.di[%d]", i);
        bool inv;
        long db;
        if (!check_keys(it, p, keys, 2, err) || !get_bool(it, "invert", p, false, &inv, err) ||
            !get_int(it, "debounce_ms", p, false, GW_DI_DEBOUNCE_DEF_MS, 0, GW_DI_DEBOUNCE_MAX_MS,
                     &db, err)) {
            return false;
        }
        if (inv) {
            cfg->local_io.di_invert |= (uint8_t)(1u << i);
        }
        cfg->local_io.di_debounce_ms[i] = (uint16_t)db;
    }
    return true;
}

/* #210: local_io.do_pulse_ms [0, 500, ...], DO/1 first: 0 = a normal DO, 50 to 60000 = a pulse
 * DO. Called once do_failsafe is parsed: a pulse DO cannot have the safe state "on". */
static bool parse_pulse(const cJSON *local_io, gw_config_t *cfg, gw_cfg_err_t *err)
{
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(local_io, "do_pulse_ms");
    if (list == NULL) {
        return true;
    }
    if (!cJSON_IsArray(list)) {
        return fail(err, "local_io.do_pulse_ms", "expected an array (DO/1 first)");
    }
    int n = cJSON_GetArraySize(list);
    if (n > GW_LOCAL_IO_CHANNELS) {
        return fail(err, "local_io.do_pulse_ms", "%d entries; there are %d outputs", n,
                    GW_LOCAL_IO_CHANNELS);
    }
    int i = 0;
    for (const cJSON *it = list->child; it != NULL; it = it->next, i++) {
        char p[40];
        snprintf(p, sizeof(p), "local_io.do_pulse_ms[%d]", i);
        if (!cJSON_IsNumber(it) || it->valuedouble != floor(it->valuedouble)) {
            return fail(err, p, "expected an integer");
        }
        double v = it->valuedouble;
        if (v != 0 && (v < GW_DO_PULSE_MIN_MS || v > GW_DO_PULSE_MAX_MS)) {
            return fail(err, p, "value %.0f: 0 (a normal DO) or %d to %d ms", v, GW_DO_PULSE_MIN_MS,
                        GW_DO_PULSE_MAX_MS);
        }
        if (v != 0 && cfg->local_io.do_failsafe[i] == GW_DO_SAFE_ON) {
            char q[40];
            snprintf(q, sizeof(q), "local_io.do_failsafe[%d]", i);
            return fail(err, q, "not for a pulse DO. Use off or hold");
        }
        cfg->local_io.do_pulse_ms[i] = (uint16_t)v;
    }
    return true;
}

/* #218: local_io.fast_counters [{"di": 1-8, "glitch_ns": 1000}], one PCNT unit each. */
static bool parse_fast(const cJSON *local_io, gw_config_t *cfg, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"di", "glitch_ns"};
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(local_io, "fast_counters");
    if (list == NULL) {
        return true;
    }
    if (!cJSON_IsArray(list)) {
        return fail(err, "local_io.fast_counters", "expected an array");
    }
    int n = cJSON_GetArraySize(list);
    if (n > GW_FAST_COUNTERS_MAX) {
        return fail(err, "local_io.fast_counters", "%d entries; the maximum is %d (PCNT units)", n,
                    GW_FAST_COUNTERS_MAX);
    }
    int i = 0;
    for (const cJSON *it = list->child; it != NULL; it = it->next, i++) {
        char p[40];
        snprintf(p, sizeof(p), "local_io.fast_counters[%d]", i);
        long di, ns;
        if (!check_keys(it, p, keys, 2, err) ||
            !get_int(it, "di", p, true, 0, 1, GW_LOCAL_IO_CHANNELS, &di, err) ||
            !get_int(it, "glitch_ns", p, false, GW_GLITCH_DEF_NS, 0, GW_GLITCH_MAX_NS, &ns, err)) {
            return false;
        }
        if (cfg->local_io.fast_mask & (1u << (di - 1))) {
            char q[48];
            snprintf(q, sizeof(q), "%s.di", p);
            return fail(err, q, "DI/%ld is already a fast counter", di);
        }
        cfg->local_io.fast_mask |= (uint8_t)(1u << (di - 1));
        cfg->local_io.glitch_ns[di - 1] = (uint16_t)ns;
    }
    return true;
}

/* #217: local_io.run_hours ["DI/3", "DO/1"]: the channels that count their time in the on state. */
static bool parse_run_hours(const cJSON *local_io, gw_config_t *cfg, gw_cfg_err_t *err)
{
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(local_io, "run_hours");
    if (list == NULL) {
        return true;
    }
    if (!cJSON_IsArray(list)) {
        return fail(err, "local_io.run_hours", "expected an array");
    }
    int i = 0;
    for (const cJSON *it = list->child; it != NULL; it = it->next, i++) {
        char p[40];
        snprintf(p, sizeof(p), "local_io.run_hours[%d]", i);
        const char *s = cJSON_IsString(it) ? it->valuestring : "";
        bool di = strncmp(s, "DI/", 3) == 0, dout = strncmp(s, "DO/", 3) == 0;
        if (!(di || dout) || s[3] < '1' || s[3] > '0' + GW_LOCAL_IO_CHANNELS || s[4] != '\0') {
            return fail(err, p, "invalid value (expected DI/1 to DI/8 or DO/1 to DO/8)");
        }
        uint16_t bit = (uint16_t)(1u << ((dout ? GW_LOCAL_IO_CHANNELS : 0) + (s[3] - '1')));
        if (cfg->local_io.run_hours & bit) {
            return fail(err, p, "duplicate channel %s", s);
        }
        cfg->local_io.run_hours |= bit;
    }
    return true;
}

static bool parse_local_io(const cJSON *root, gw_config_t *cfg, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"di",          "failsafe_s",     "do_failsafe",
                                       "do_pulse_ms", "counter_save_s", "fast_counters",
                                       "run_hours",   "buzzer",         "rules"};
    static const char *const safe_names[] = {"off", "on", "hold"};   /* gw_do_safe_t order */
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, "local_io");
    static const cJSON empty = {.type = cJSON_Object};
    long fs, save;
    if (o == NULL) {
        o = &empty;
    }
    if (!check_keys(o, "local_io", keys, sizeof(keys) / sizeof(keys[0]), err) ||
        !get_int(o, "failsafe_s", "local_io", false, 0, 0, GW_FAILSAFE_MAX_S, &fs, err) ||
        !get_int(o, "counter_save_s", "local_io", false, 600, 0, GW_COUNTER_SAVE_MAX_S, &save,
                 err)) {
        return false;
    }
    if (save != 0 && save < GW_COUNTER_SAVE_MIN_S) {
        return fail(err, "local_io.counter_save_s",
                    "value %ld: 0 (do not save) or at least %d s (wear of the flash)", save,
                    GW_COUNTER_SAVE_MIN_S);
    }
    cfg->local_io.failsafe_s = (uint32_t)fs;
    cfg->local_io.counter_save_s = (uint32_t)save;
    if (!parse_di(o, cfg, err)) {
        return false;
    }
    /* IO-3: one safe state per DO, in order (DO/1 first); missing entries are "off" */
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(o, "do_failsafe");
    if (list != NULL && !cJSON_IsArray(list)) {
        return fail(err, "local_io.do_failsafe", "expected an array (DO/1 first)");
    }
    int n = list != NULL ? cJSON_GetArraySize(list) : 0;
    if (n > GW_LOCAL_IO_CHANNELS) {
        return fail(err, "local_io.do_failsafe", "%d entries; there are %d outputs", n,
                    GW_LOCAL_IO_CHANNELS);
    }
    int i = 0;
    for (const cJSON *it = list != NULL ? list->child : NULL; it != NULL; it = it->next, i++) {
        char p[40];
        snprintf(p, sizeof(p), "local_io.do_failsafe[%d]", i);
        int mode = -1;
        for (int k = 0; cJSON_IsString(it) && k < 3; k++) {
            if (strcmp(it->valuestring, safe_names[k]) == 0) {
                mode = k;
            }
        }
        if (mode < 0) {
            return fail(err, p, "invalid value (expected off|on|hold)");
        }
        cfg->local_io.do_failsafe[i] = (uint8_t)mode;
    }
    return parse_pulse(o, cfg, err) && parse_fast(o, cfg, err) && parse_run_hours(o, cfg, err) &&
           parse_buzzer(o, cfg, err) && parse_rules(o, cfg, err);
}

static bool parse_syslog(const cJSON *root, gw_config_t *cfg, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"host", "port", "level"};
    static const char *const levels[] = {"error", "warning", "info", "debug"};
    static const gw_log_level_t level_values[] = {GW_LOG_ERROR, GW_LOG_WARNING, GW_LOG_INFO,
                                                  GW_LOG_DEBUG};
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, "syslog");
    static const cJSON empty = {.type = cJSON_Object};
    long port;
    int level;
    if (o == NULL) {
        o = &empty;
    }
    if (!check_keys(o, "syslog", keys, 3, err) ||
        !get_string(o, "host", "syslog", false, "", cfg->syslog.host, sizeof(cfg->syslog.host),
                    err) ||
        !get_int(o, "port", "syslog", false, 514, 1, 65535, &port, err) ||
        !get_enum(o, "level", "syslog", false, 2, levels, 4, &level, err)) {
        return false;
    }
    cfg->syslog.port = (uint16_t)port;
    cfg->syslog.level = level_values[level];
    return true;
}

/* #206: "gateway": {"name": "", "location": "", "comment": ""}, all optional. The web UI shows
 * the name and the location in its header and in the title of the browser tab, and the NBIRTH
 * gives them as Node Info/Name and Node Info/Location (Sparkplug strings: valid UTF-8, issue
 * #162). The comment is only in the document (the Configuration tab): it is validated, not kept. */
static bool check_label(const char *s, const char *path, gw_cfg_err_t *err)
{
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        if (*c < 0x20 || *c == 0x7f) {
            return fail(err, path, "control characters are not allowed");
        }
    }
    return check_utf8(s, path, err);
}

static bool parse_gateway(const cJSON *root, gw_config_t *cfg, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"name", "location", "comment"};
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, "gateway");
    static const cJSON empty = {.type = cJSON_Object};
    char comment[GW_COMMENT_MAX + 1];
    if (o == NULL) {
        o = &empty;
    }
    return check_keys(o, "gateway", keys, 3, err) &&
           get_string(o, "name", "gateway", false, "", cfg->gateway.name,
                      sizeof(cfg->gateway.name), err) &&
           check_label(cfg->gateway.name, "gateway.name", err) &&
           get_string(o, "location", "gateway", false, "", cfg->gateway.location,
                      sizeof(cfg->gateway.location), err) &&
           check_label(cfg->gateway.location, "gateway.location", err) &&
           get_string(o, "comment", "gateway", false, "", comment, sizeof(comment), err) &&
           check_utf8(comment, "gateway.comment", err);
}

/* FW-1: "ota": {"mqtt": false, "ca_file": ""}. #223: "mqtt" lets a Sparkplug host start an
 * update (NCMD); an update from the web UI is always possible. The old key "enabled" is unknown
 * now (no migration before v1.0). */
static bool parse_ota(const cJSON *root, gw_config_t *cfg, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"mqtt", "ca_file"};
    static const cJSON empty = {.type = cJSON_Object};
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, "ota");
    if (o == NULL) {
        o = &empty;
    }
    return check_keys(o, "ota", keys, 2, err) &&
           get_bool(o, "mqtt", "ota", false, &cfg->ota.mqtt, err) &&
           get_file_name(o, "ca_file", "ota", cfg->ota.ca_file, err);
}

/* MB-15. Modbus TCP has no authentication: off by default, and allowed_clients limits who may
 * connect ("a.b.c.d" or "a.b.c.d/prefix"). */
static bool parse_modbus_tcp(const cJSON *root, gw_config_t *cfg, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"enabled", "port", "allowed_clients", "idle_timeout_s",
                                       "local_unit_id", "local_writes"};
    static const cJSON empty = {.type = cJSON_Object};
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, "modbus_tcp");
    if (o == NULL) {
        o = &empty;
    }
    long port, idle, local;
    if (!check_keys(o, "modbus_tcp", keys, 6, err) ||
        !get_bool(o, "enabled", "modbus_tcp", false, &cfg->modbus_tcp.enabled, err) ||
        !get_int(o, "port", "modbus_tcp", false, 502, 1, 65535, &port, err) ||
        !get_int(o, "idle_timeout_s", "modbus_tcp", false, 60, 0, 3600, &idle, err) ||
        !get_int(o, "local_unit_id", "modbus_tcp", false, 0, 0, 247, &local, err) ||
        !get_bool(o, "local_writes", "modbus_tcp", false, &cfg->modbus_tcp.local_writes, err)) {
        return false;
    }
    cfg->modbus_tcp.port = (uint16_t)port;
    cfg->modbus_tcp.idle_timeout_s = (uint16_t)idle;
    /* #216: the gateway answers this unit itself and never sends it to the bus, so no device
     * may have it (the devices are parsed before this section) */
    for (uint16_t d = 0; local != 0 && d < cfg->device_count; d++) {
        if (cfg->devices[d].unit_id == local) {
            return fail(err, "modbus_tcp.local_unit_id", "unit %ld is the Unit ID of the device %s",
                        local, cfg->devices[d].name);
        }
    }
    cfg->modbus_tcp.local_unit_id = (uint8_t)local;
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(o, "allowed_clients");
    if (list == NULL) {
        return true;
    }
    if (!cJSON_IsArray(list)) {
        return fail(err, "modbus_tcp.allowed_clients", "expected an array");
    }
    int n = cJSON_GetArraySize(list);
    if (n > GW_MBTCP_MAX_ALLOWED) {
        return fail(err, "modbus_tcp.allowed_clients", "%d entries; the maximum is %d", n,
                    GW_MBTCP_MAX_ALLOWED);
    }
    int i = 0;
    for (const cJSON *it = list->child; it != NULL; it = it->next, i++) {
        char p[48];
        snprintf(p, sizeof(p), "modbus_tcp.allowed_clients[%d]", i);
        if (!cJSON_IsString(it)) {
            return fail(err, p, "expected a string (\"a.b.c.d\" or \"a.b.c.d/n\")");
        }
        char buf[24];
        size_t len = strlen(it->valuestring);
        if (len >= sizeof(buf)) {
            return fail(err, p, "'%s' is not an IPv4 address or network", it->valuestring);
        }
        memcpy(buf, it->valuestring, len + 1);
        unsigned prefix = 32;
        char *slash = strchr(buf, '/');
        if (slash != NULL) {
            char tail;
            *slash = '\0';
            if (sscanf(slash + 1, "%u%c", &prefix, &tail) != 1 || prefix > 32) {
                return fail(err, p, "invalid prefix in '%s' (0-32)", it->valuestring);
            }
        }
        uint8_t *ip = cfg->modbus_tcp.clients[i].ip;
        if (!parse_ipv4(buf, ip)) {
            return fail(err, p, "'%s' is not an IPv4 address or network", it->valuestring);
        }
        cfg->modbus_tcp.clients[i].prefix = (uint8_t)prefix;
    }
    cfg->modbus_tcp.client_count = (uint8_t)n;
    return true;
}

/* CFG-3 / CFG-4: web UI and REST API. The password is a secret (CFG-7): it only appears
 * here while a new one is being set; the stored document never holds it. */
static bool parse_web(const cJSON *root, gw_config_t *cfg, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"enabled", "port", "username", "password", "https",
                                       "https_port"};
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, "web");
    static const cJSON empty = {.type = cJSON_Object};
    long port, sport;
    if (o == NULL) {
        o = &empty;
    }
    if (!check_keys(o, "web", keys, 6, err) ||
        !get_bool(o, "enabled", "web", true, &cfg->web.enabled, err) ||
        !get_int(o, "port", "web", false, 80, 1, 65535, &port, err) ||
        !get_bool(o, "https", "web", true, &cfg->web.https, err) ||
        !get_int(o, "https_port", "web", false, 443, 1, 65535, &sport, err) ||
        !get_string(o, "username", "web", false, "admin", cfg->web.username,
                    sizeof(cfg->web.username), err) ||
        !get_string(o, "password", "web", false, "", cfg->web.password,
                    sizeof(cfg->web.password), err)) {
        return false;
    }
    cfg->web.port = (uint16_t)port;
    cfg->web.https_port = (uint16_t)sport;
    if (cfg->web.https && sport == port) {
        return fail(err, "web.https_port", "cannot be equal to web.port (%ld)", port);
    }
    if (cfg->web.username[0] == '\0') {
        return fail(err, "web.username", "cannot be empty");
    }
    for (const unsigned char *c = (const unsigned char *)cfg->web.username; *c; c++) {
        if (*c <= 0x20 || *c >= 0x7f || *c == ':') {
            return fail(err, "web.username",
                        "only visible ASCII characters, no ':' (Basic auth)");
        }
    }
    /* issue #187: the web password is also the WPA2 key of the access point (its PMK): the
     * rule of a WPA2 passphrase (gw_wpa.h). GW_SECRET_MASK obeys it. */
    switch (cfg->web.password[0] == '\0' ? GW_WPA_PASS_OK : gw_wpa_pass_check(cfg->web.password)) {
    case GW_WPA_PASS_OK:
        break;
    case GW_WPA_PASS_SHORT:
        return fail(err, "web.password", "expected at least 8 characters (or empty)");
    case GW_WPA_PASS_LONG:
        return fail(err, "web.password", "expected at most %d characters: it is also the WPA2 "
                    "key of the access point", GW_WPA_PASS_MAX);
    case GW_WPA_PASS_BAD_CHAR:
        return fail(err, "web.password", "only printable ASCII characters (a space to '~'): it "
                    "is also the WPA2 key of the access point");
    }
    return true;
}

/* The shortest text of @p v that reads back as the same double (1500, 0.1, 9007199254740993). */
static const char *num_text(double v, char *buf, size_t cap)
{
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(buf, cap, "%.*g", prec, v);
        if (strtod(buf, NULL) == v) {
            break;
        }
    }
    return buf;
}

static bool parse_tag(const cJSON *o, const char *path, gw_tag_cfg_t *t, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"name",   "area",     "address",  "type",  "word_order",
                                       "scale",  "offset",   "deadband", "writable", "units",
                                       "bit_offset", "bit_width", "length", "scan", "eng_low",
                                       "eng_high", "write_min", "write_max"};
    static const char *const scans[] = {"fast", "slow"};
    int area, type, word, scan;
    long addr, bit, width, length;
    double eng_low, eng_high;
    char p[96];
    if (!check_keys(o, path, keys, sizeof(keys) / sizeof(keys[0]), err) ||
        !get_string(o, "name", path, true, "", t->name, sizeof(t->name), err) ||
        !get_enum(o, "area", path, true, 0, k_area_names, 4, &area, err) ||
        !get_int(o, "address", path, true, 0, 0, 65535, &addr, err) ||
        !get_enum(o, "type", path, true, 0, k_type_names, TYPE_COUNT, &type, err) ||
        !get_enum(o, "word_order", path, false, 0, k_word_names, 6, &word, err) ||
        !get_double(o, "scale", path, 1.0, &t->scale, err) ||
        !get_double(o, "offset", path, 0.0, &t->offset, err) ||
        !get_double(o, "deadband", path, 0.0, &t->deadband, err) ||
        !get_bool(o, "writable", path, false, &t->writable, err) ||
        !get_string(o, "units", path, false, "", t->units, sizeof(t->units), err) ||
        !get_int(o, "bit_offset", path, false, GW_NO_BIT, 0, 15, &bit, err) ||
        !get_int(o, "bit_width", path, false, 1, 1, 16, &width, err) ||
        !get_int(o, "length", path, false, 0, 1, GW_STR_MAX_REGS, &length, err) ||
        !get_enum(o, "scan", path, false, 0, scans, 2, &scan, err) ||
        !get_double(o, "eng_low", path, 0.0, &eng_low, err) ||
        !get_double(o, "eng_high", path, 0.0, &eng_high, err) ||
        !get_double(o, "write_min", path, 0.0, &t->write_min, err) ||
        !get_double(o, "write_max", path, 0.0, &t->write_max, err)) {
        return false;
    }
    t->has_write_min = cJSON_GetObjectItemCaseSensitive(o, "write_min") != NULL;
    t->has_write_max = cJSON_GetObjectItemCaseSensitive(o, "write_max") != NULL;
    if (fabs(eng_low) > FLT_MAX || fabs(eng_high) > FLT_MAX) {
        join(p, sizeof(p), path, fabs(eng_low) > FLT_MAX ? "eng_low" : "eng_high");
        return fail(err, p, "out of the range of a Float (±3.4e38)");
    }
    t->eng_low = (float)eng_low;
    t->eng_high = (float)eng_high;
    t->has_eng_low = cJSON_GetObjectItemCaseSensitive(o, "eng_low") != NULL;
    t->has_eng_high = cJSON_GetObjectItemCaseSensitive(o, "eng_high") != NULL;
    t->slow = scan == 1;
    t->area = (gw_area_t)area;
    t->type = (gw_type_t)type;
    t->word_order = k_word_values[word];
    t->address = (uint16_t)addr;
    t->bit_offset = (int8_t)bit;
    t->length = (uint8_t)length;

    if (t->name[0] == '\0') {
        join(p, sizeof(p), path, "name");
        return fail(err, p, "cannot be empty");
    }
    /* issue #162: the metric name and the engUnit property are Sparkplug strings */
    join(p, sizeof(p), path, "name");
    if (!check_utf8(t->name, p, err)) {
        return false;
    }
    join(p, sizeof(p), path, "units");
    if (!check_utf8(t->units, p, err)) {
        return false;
    }
    bool bit_area = t->area == GW_AREA_COIL || t->area == GW_AREA_DISCRETE;
    if (bit_area && t->type != GW_TYPE_BOOL) {
        join(p, sizeof(p), path, "type");
        return fail(err, p, "the coil/discrete areas accept only type=bool");
    }
    /* MB-6 #202: one bit (type bool) or a field of 2 to 16 bits (type uint16) of a register */
    bool has_bit = cJSON_GetObjectItemCaseSensitive(o, "bit_offset") != NULL;
    bool has_width = cJSON_GetObjectItemCaseSensitive(o, "bit_width") != NULL;
    if (has_width && !has_bit) {
        join(p, sizeof(p), path, "bit_width");
        return fail(err, p, "valid only with bit_offset");
    }
    if (has_bit && bit_area) {
        join(p, sizeof(p), path, "bit_offset");
        return fail(err, p, "valid only in the holding and input areas");
    }
    if (!bit_area && t->type == GW_TYPE_BOOL && !has_bit) {
        join(p, sizeof(p), path, "type");
        return fail(err, p, "type=bool in holding/input needs the key bit_offset (0-15); "
                            "without bit_offset, it is valid only in the coil and discrete areas");
    }
    if (has_bit && bit + width > 16) {
        join(p, sizeof(p), path, "bit_width");
        return fail(err, p, "bit_offset + bit_width must be 16 or less (%ld + %ld = %ld)", bit,
                    width, bit + width);
    }
    if (has_bit && width == 1 && t->type != GW_TYPE_BOOL) {
        join(p, sizeof(p), path, "type");
        return fail(err, p, "a field of 1 bit (bit_width 1, the default) needs type=bool; a "
                            "field of 2 to 16 bits needs type=uint16 and bit_width");
    }
    if (has_bit && width > 1 && t->type != GW_TYPE_UINT16) {
        join(p, sizeof(p), path, "type");
        return fail(err, p, "a field of %ld bits (bit_width) needs type=uint16", width);
    }
    t->bit_width = has_bit ? (uint8_t)width : 0;
    if (has_bit && cJSON_GetObjectItemCaseSensitive(o, "word_order") != NULL &&
        t->word_order != GW_WORD_BIG) {
        join(p, sizeof(p), path, "word_order");
        return fail(err, p, "not accepted with bit_offset: bit 0 is the least significant bit "
                            "of the register as it arrives");
    }
    bool has_length = cJSON_GetObjectItemCaseSensitive(o, "length") != NULL;
    bool blob = t->type == GW_TYPE_STRING || t->type == GW_TYPE_BYTES;
    if (blob != has_length) {
        join(p, sizeof(p), path, "length");
        return fail(err, p, has_length ? "valid only with type=string or bytes"
                                       : "required with type=%s (registers, 1-16)",
                    k_type_names[t->type]);
    }
    if (t->writable && t->area != GW_AREA_COIL && t->area != GW_AREA_HOLDING) {
        join(p, sizeof(p), path, "writable");
        return fail(err, p, "only the coil and holding areas are writable");
    }
    if ((uint32_t)t->address + gw_tag_width(t) - 1u > 65535u) {
        join(p, sizeof(p), path, "address");
        return fail(err, p, "the tag goes past the address 65535");
    }
    if (t->scale == 0.0) {
        join(p, sizeof(p), path, "scale");
        return fail(err, p, "cannot be 0");
    }
    if ((t->type == GW_TYPE_BOOL || blob) && (t->scale != 1.0 || t->offset != 0.0)) {
        join(p, sizeof(p), path, "scale");
        return fail(err, p, "scale/offset not accepted in %s tags", k_type_names[t->type]);
    }
    if (t->deadband < 0.0) {
        join(p, sizeof(p), path, "deadband");
        return fail(err, p, "cannot be negative");
    }
    if (blob && t->deadband != 0.0) {
        join(p, sizeof(p), path, "deadband");
        return fail(err, p, "not accepted in %s tags", k_type_names[t->type]);
    }
    /* SPB-16: the engineering range of a numeric value */
    if ((t->has_eng_low || t->has_eng_high) && (t->type == GW_TYPE_BOOL || blob)) {
        join(p, sizeof(p), path, t->has_eng_low ? "eng_low" : "eng_high");
        return fail(err, p, "not accepted in %s tags", k_type_names[t->type]);
    }
    if (t->has_eng_low && t->has_eng_high && t->eng_low >= t->eng_high) {
        join(p, sizeof(p), path, "eng_high");
        return fail(err, p, "must be greater than eng_low (%g)", t->eng_low);
    }
    /* #196: the range of a DCMD write, in engineering units */
    if (t->has_write_min || t->has_write_max) {
        join(p, sizeof(p), path, t->has_write_min ? "write_min" : "write_max");
        if (t->type == GW_TYPE_BOOL || blob) {
            return fail(err, p, "not accepted in %s tags", k_type_names[t->type]);
        }
        if (!t->writable) {
            return fail(err, p, "valid only with writable=true");
        }
    }
    if (t->has_write_min && t->has_write_max && t->write_max < t->write_min) {
        join(p, sizeof(p), path, "write_max");
        char hi[32], lo[32];
        return fail(err, p, "%s is less than write_min (%s)", num_text(t->write_max, hi, sizeof(hi)),
                    num_text(t->write_min, lo, sizeof(lo)));
    }
    return true;
}

/* The tags of the array @p tags at @p path ("devices[0].tags", "device_types[1].tags") into
 * cfg->tags from cfg->tag_count on; cfg->tag_count grows by their number (@p count). */
static bool parse_tags(const cJSON *tags, const char *path, gw_config_t *cfg, uint16_t *count,
                       gw_cfg_err_t *err)
{
    char p[96];
    if (!cJSON_IsArray(tags)) {
        return fail(err, path, tags == NULL ? "missing required field" : "expected an array");
    }
    int n = cJSON_GetArraySize(tags);
    if (n == 0) {
        return fail(err, path, "the list has no tags");
    }
    if (n > GW_MAX_TAGS_PER_DEVICE) {
        return fail(err, path, "%d tags; the maximum per device is %d", n, GW_MAX_TAGS_PER_DEVICE);
    }
    if (cfg->tag_count + n > GW_MAX_TAGS) {
        return fail(err, path, "more than the total maximum of %d tags", GW_MAX_TAGS);
    }
    uint16_t first = cfg->tag_count;
    int i = 0;
    for (const cJSON *it = tags->child; it != NULL; it = it->next, i++) {
        char tp[96];
        snprintf(tp, sizeof(tp), "%s[%d]", path, i);
        gw_tag_cfg_t *t = &cfg->tags[cfg->tag_count];
        if (!parse_tag(it, tp, t, err)) {
            return false;
        }
        join(p, sizeof(p), tp, "name");
        for (uint16_t j = first; j < cfg->tag_count; j++) {
            const char *other = cfg->tags[j].name;
            unsigned k = (unsigned)(j - first);
            if (strcmp(other, t->name) == 0) {
                return fail(err, p, "duplicate tag name '%s'", t->name);
            }
            /* issue #162: tck-id-case-sensitivity-metric-names */
            if (same_but_case(other, t->name)) {
                return fail(err, p, "'%s' differs only in case from tags[%u] '%s' (Sparkplug "
                                    "metric names should not)", t->name, k, other);
            }
            if (folder_of(other, t->name) || folder_of(t->name, other)) {
                return fail(err, p, "'%s' and tags[%u] '%s': a metric name cannot also be a "
                                    "folder of another metric", t->name, k, other);
            }
        }
        cfg->tag_count++;
    }
    *count = (uint16_t)(cfg->tag_count - first);
    return true;
}

/* The name of device_types[@p i] (validated by parse_device_types), "" if none. */
static const char *type_name_at(const cJSON *types, int i)
{
    const cJSON *t = cJSON_GetArrayItem(types, i);
    const cJSON *v = t != NULL ? cJSON_GetObjectItemCaseSensitive(t, "name") : NULL;
    return cJSON_IsString(v) && v->valuestring != NULL ? v->valuestring : "";
}

/* #213: device_types, a list of {name, tags}. A device with "type" uses the tags of its type
 * (parse_device copies them), so the tag list of a model is in the document one time. Each type
 * that a device uses is one Sparkplug template definition in the NBIRTH (SPB-18, gw_node.c):
 * its name is the name of an NBIRTH metric. Nothing is kept here: the tags of each type are
 * examined in the free part of cfg->tags, then cleared. */
static bool parse_device_types(const cJSON *root, gw_config_t *cfg, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"name", "tags"};
    /* The definition is an NBIRTH metric named <type>, next to the node's own metrics (gw_node):
     * not one of them, nor one of their folders, also without case (issue #162). "Run Hours" is
     * the folder of Run Hours/<DI|DO>/n (#217): a definition with that name would also be a
     * folder of these metrics. */
    static const char *const node_names[] = {"bdSeq", "DI", "DO", "Counter", "Node Control",
                                             "Node Info", "Firmware Update", "Run Hours"};
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "device_types");
    if (arr == NULL) {
        return true;
    }
    if (!cJSON_IsArray(arr)) {
        return fail(err, "device_types", "expected an array");
    }
    int n = cJSON_GetArraySize(arr);
    if (n > GW_MAX_DEVICE_TYPES) {
        return fail(err, "device_types", "%d device types; the maximum is %d", n,
                    GW_MAX_DEVICE_TYPES);
    }
    int i = 0;
    for (const cJSON *it = arr->child; it != NULL; it = it->next, i++) {
        char path[32], p[48];
        char name[GW_NAME_SIZE];
        snprintf(path, sizeof(path), "device_types[%d]", i);
        if (!check_keys(it, path, keys, sizeof(keys) / sizeof(keys[0]), err) ||
            !get_string(it, "name", path, true, "", name, sizeof(name), err)) {
            return false;
        }
        snprintf(p, sizeof(p), "%s.name", path);
        if (!check_topic_id(name, p, false, err)) {
            return false;
        }
        for (size_t k = 0; k < sizeof(node_names) / sizeof(node_names[0]); k++) {
            if (strcasecmp(name, node_names[k]) == 0) {
                return fail(err, p, "'%s' is the name or a folder of the node metric '%s'", name,
                            node_names[k]);
            }
        }
        for (int j = 0; j < i; j++) {
            const char *o = type_name_at(arr, j);
            if (strcmp(o, name) == 0) {
                return fail(err, p, "duplicate device type name '%s'", name);
            }
            /* issue #162: tck-id-case-sensitivity-metric-names */
            if (same_but_case(o, name)) {
                return fail(err, p, "'%s' differs only in case from device_types[%d] '%s' "
                                    "(Sparkplug metric names should not)", name, j, o);
            }
        }
        snprintf(p, sizeof(p), "%s.tags", path);
        uint16_t count = 0;
        bool ok = parse_tags(cJSON_GetObjectItemCaseSensitive(it, "tags"), p, cfg, &count, err);
        memset(cfg->tags, 0, sizeof(cfg->tags[0]) * cfg->tag_count);
        cfg->tag_count = 0;
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* #213: the index in device_types of the type @p name, or GW_NO_TYPE. */
static int find_type(const cJSON *root, const char *name)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "device_types");
    int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(type_name_at(arr, i), name) == 0) {
            return i;
        }
    }
    return GW_NO_TYPE;
}

static bool parse_device(const cJSON *root, const cJSON *o, const char *path, gw_config_t *cfg,
                         gw_device_cfg_t *d, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"name",        "unit_id", "poll_ms", "slow_poll_ms",
                                       "offline_after", "block_reads", "max_gap", "tags", "type",
                                       "write_multiple", "enabled"};
    long unit, poll, off, gap, slow;
    bool merge, multi;
    char p[96];
    if (!check_keys(o, path, keys, sizeof(keys) / sizeof(keys[0]), err) ||
        !get_string(o, "name", path, true, "", d->name, sizeof(d->name), err) ||
        !get_string(o, "type", path, false, "", d->type, sizeof(d->type), err) ||
        !get_int(o, "unit_id", path, true, 0, 1, 247, &unit, err) ||
        !get_int(o, "poll_ms", path, false, 1000, 50, 3600000, &poll, err) ||
        !get_int(o, "slow_poll_ms", path, false, poll * 10 < 3600000 ? poll * 10 : 3600000, 50,
                 3600000, &slow, err) ||
        !get_int(o, "offline_after", path, false, 3, 1, 1000, &off, err) ||
        !get_bool(o, "block_reads", path, true, &merge, err) ||
        !get_int(o, "max_gap", path, false, GW_CFG_BUILD_DEFAULT, 0, GW_MAX_GAP_LIMIT, &gap, err) ||
        !get_bool(o, "write_multiple", path, false, &multi, err) ||
        !get_bool(o, "enabled", path, true, &d->enabled, err)) {
        return false;
    }
    /* #197: an absent key keeps the build's setting (CONFIG_GW_MB_FORCE_MULTI_WRITE) */
    d->write_multiple = cJSON_GetObjectItemCaseSensitive(o, "write_multiple") != NULL
                            ? (int8_t)multi
                            : (int8_t)GW_CFG_BUILD_DEFAULT;
    /* MB-11: absent keys keep the build's setting (CONFIG_GW_MB_BLOCK_READS / _MAX_GAP) */
    bool has_merge = cJSON_GetObjectItemCaseSensitive(o, "block_reads") != NULL;
    d->block_reads = has_merge ? (int8_t)merge : (int8_t)GW_CFG_BUILD_DEFAULT;
    d->max_gap = (int16_t)gap;
    if (has_merge && !merge && gap != GW_CFG_BUILD_DEFAULT) {
        join(p, sizeof(p), path, "max_gap");
        return fail(err, p, "not permitted with block_reads=false");
    }
    join(p, sizeof(p), path, "name");
    if (!check_topic_id(d->name, p, false, err)) {
        return false;
    }
    if (slow < poll) {
        join(p, sizeof(p), path, "slow_poll_ms");
        return fail(err, p, "cannot be less than poll_ms (%ld)", poll);
    }
    d->unit_id = (uint8_t)unit;
    d->poll_ms = (uint32_t)poll;
    d->slow_poll_ms = (uint32_t)slow;
    d->offline_after = (uint16_t)off;

    d->first_tag = cfg->tag_count;
    d->tag_count = 0;
    d->type_index = GW_NO_TYPE;
    const cJSON *tags = cJSON_GetObjectItemCaseSensitive(o, "tags");
    if (d->type[0] == '\0') {
        join(p, sizeof(p), path, "tags");
        return parse_tags(tags, p, cfg, &d->tag_count, err);
    }
    /* #213: a device with a type uses the tags of the type, and has none of its own */
    if (tags != NULL) {
        join(p, sizeof(p), path, "tags");
        return fail(err, p, "not permitted with type: the device uses the tags of the device "
                            "type '%s'", d->type);
    }
    join(p, sizeof(p), path, "type");
    int ti = find_type(root, d->type);
    if (ti == GW_NO_TYPE) {
        return fail(err, p, "no device type '%s' in device_types", d->type);
    }
    const cJSON *tt = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "device_types"), ti), "tags");
    int n = cJSON_GetArraySize(tt);
    if (cfg->tag_count + n > GW_MAX_TAGS) {
        return fail(err, p, "the %d tags of the device type '%s' make more than the total "
                            "maximum of %d tags", n, d->type, GW_MAX_TAGS);
    }
    d->type_index = (int8_t)ti;
    /* the copy of the tags of the type, examined by parse_device_types: the schema report
     * (FW-1) has their keys already */
    char tp[48];
    snprintf(tp, sizeof(tp), "device_types[%d].tags", ti);
    gw_schema_report_t *rep = s_rep;
    s_rep = NULL;
    bool ok = parse_tags(tt, tp, cfg, &d->tag_count, err);
    s_rep = rep;
    return ok;
}

static bool parse_devices(const cJSON *root, gw_config_t *cfg, gw_cfg_err_t *err)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (arr == NULL) {
        return true;
    }
    if (!cJSON_IsArray(arr)) {
        return fail(err, "devices", "expected an array");
    }
    int n = cJSON_GetArraySize(arr);
    if (n > GW_MAX_DEVICES) {
        return fail(err, "devices", "%d devices; the maximum is %d", n, GW_MAX_DEVICES);
    }
    unsigned template_tags = 0;
    int i = 0;
    for (const cJSON *it = arr->child; it != NULL; it = it->next, i++) {
        char path[32];
        snprintf(path, sizeof(path), "devices[%d]", i);
        gw_device_cfg_t *d = &cfg->devices[cfg->device_count];
        if (!parse_device(root, it, path, cfg, d, err)) {
            return false;
        }
        for (uint16_t j = 0; j < cfg->device_count; j++) {
            char p[48];
            snprintf(p, sizeof(p), "%s.name", path);
            if (strcmp(cfg->devices[j].name, d->name) == 0) {
                return fail(err, p, "duplicate device name '%s'", d->name);
            }
            /* issue #162: tck-id-case-sensitivity-sparkplug-ids */
            if (same_but_case(cfg->devices[j].name, d->name)) {
                return fail(err, p, "'%s' differs only in case from devices[%u] '%s' (Sparkplug "
                                    "device IDs should not)", d->name, j, cfg->devices[j].name);
            }
        }
        /* SPB-18: each type that a device uses is one template definition in the NBIRTH
         * (tck-id-payloads-template-definition-nbirth). The definitions together describe at
         * most GW_MAX_TEMPLATE_TAGS tags, so the NBIRTH stays bounded. */
        bool first = d->type[0] != '\0';
        for (uint16_t j = 0; first && j < cfg->device_count; j++) {
            first = strcmp(cfg->devices[j].type, d->type) != 0;
        }
        if (first && template_tags + d->tag_count > GW_MAX_TEMPLATE_TAGS) {
            char p[48];
            snprintf(p, sizeof(p), "%s.type", path);
            return fail(err, p, "the device types that the devices use describe more than %d "
                                "tags in total (the NBIRTH holds their definitions)",
                        GW_MAX_TEMPLATE_TAGS);
        }
        template_tags += first ? d->tag_count : 0u;
        /* #172: the longest topic of the device ("spBv1.0/<group>/DBIRTH/<edge>/<device_id>")
         * fits GW_TOPIC_SIZE: gw_node builds it in a buffer of that size, with no allocation */
        size_t prefix = strlen(cfg->sparkplug.device_prefix);
        size_t id_len = (prefix > 0 ? prefix + 1 : 0) + strlen(d->name);
        size_t fixed = strlen("spBv1.0/") + strlen(cfg->sparkplug.group_id) + strlen("/DBIRTH/") +
                       strlen(cfg->sparkplug.edge_node_id) + strlen("/");
        if (fixed + id_len >= GW_TOPIC_SIZE) {
            char p[48];
            snprintf(p, sizeof(p), "%s.name", path);
            return fail(err, p, "the device_id (sparkplug.device_prefix, ':' and the name) has %u "
                        "bytes; with this group_id and edge_node_id the maximum is %u (Sparkplug "
                        "topic of %d bytes)", (unsigned)id_len,
                        (unsigned)(GW_TOPIC_SIZE - 1 - fixed), GW_TOPIC_SIZE - 1);
        }
        cfg->device_count++;
    }
    return true;
}

unsigned gw_json_depth(const char *json, size_t len)
{
    unsigned depth = 0, max = 0;
    bool in_str = false, esc = false;
    for (size_t i = 0; i < len && json[i] != '\0'; i++) {
        char c = json[i];
        if (in_str) {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                in_str = false;
            }
        } else if (c == '"') {
            in_str = true;
        } else if (c == '{' || c == '[') {
            if (++depth > max) {
                max = depth;
            }
        } else if ((c == '}' || c == ']') && depth > 0) {
            depth--;
        }
    }
    return max;
}

static bool parse_locked(const char *json, size_t len, gw_config_t *cfg, gw_cfg_err_t *err);

/* #169: the first GW_LINT_ERROR becomes the error of the validation. */
static bool lint_refuse(void *ctx, gw_lint_level_t level, const char *path, const char *msg)
{
    if (level != GW_LINT_ERROR) {
        return true;
    }
    fail(ctx, path, "%s", msg);
    return false;
}

/* Issue #106: cJSON's hooks, its error pointer and s_alloc_failed are process-wide: the whole
 * parse runs under the JSON lock (another task's parse could set or clear the flag). */
bool gw_config_parse(const char *json, size_t len, gw_config_t *cfg, gw_cfg_err_t *err)
{
    gw_port_json_lock();
    bool ok = parse_locked(json, len, cfg, err);
    gw_port_json_unlock();
    return ok;
}

bool gw_config_parse_schema(const char *json, size_t len, gw_config_t *cfg, gw_cfg_err_t *err,
                            gw_schema_report_t *rep)
{
    gw_port_json_lock();
    rep->valid = false;
    s_rep = rep;
    bool ok = parse_locked(json, len, cfg, err);
    s_rep = NULL;
    rep->valid = ok;
    gw_port_json_unlock();
    return ok;
}

static bool parse_locked(const char *json, size_t len, gw_config_t *cfg, gw_cfg_err_t *err)
{
    static const char *const keys[] = {"network", "ntp",      "mqtt",    "sparkplug", "rs485",
                                       "local_io", "devices", "syslog",  "web",
                                       "modbus_tcp", "ota", "gateway", "device_types"};
    memset(cfg, 0, sizeof(*cfg));
    err->msg[0] = '\0';
    err->path[0] = '\0';
    err->line = err->column = 0;

    /* cJSON recurses once per nesting level: bound it before parsing (the schema needs 4). */
    if (gw_json_depth(json, len) > GW_JSON_MAX_DEPTH) {
        snprintf(err->msg, sizeof(err->msg), "JSON nested deeper than %d levels",
                 GW_JSON_MAX_DEPTH);
        return false;
    }
    gw_config_json_hooks();
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, len, &end, false);
    if (root == NULL && s_alloc_failed) {
        snprintf(err->msg, sizeof(err->msg), "not enough memory to parse %zu bytes of JSON",
                 len);
        return false;
    }
    if (root == NULL) {
        const char *at = cJSON_GetErrorPtr();
        return fail_at(err, json, (at != NULL && at >= json && at <= json + len) ? at : json,
                       "invalid syntax");
    }
    while (end < json + len && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        end++;
    }
    if (end < json + len && *end != '\0') {
        cJSON_Delete(root);
        return fail_at(err, json, end, "content after the document");
    }
    bool ok = check_keys(root, "", keys, sizeof(keys) / sizeof(keys[0]), err) && parse_network(root, cfg, err) &&
              parse_ntp(root, cfg, err) && parse_mqtt(root, cfg, err) &&
              parse_sparkplug(root, cfg, err) && parse_rs485(root, cfg, err) &&
              parse_local_io(root, cfg, err) && parse_device_types(root, cfg, err) &&
              parse_devices(root, cfg, err) &&
              parse_syslog(root, cfg, err) && parse_web(root, cfg, err) &&
              parse_modbus_tcp(root, cfg, err) && parse_ota(root, cfg, err) &&
              parse_gateway(root, cfg, err);
    cJSON_Delete(root);

    if (ok && cfg->mqtt.client_id[0] == '\0') {
        strcpy(cfg->mqtt.client_id, cfg->sparkplug.edge_node_id);
    }
    if (ok && cfg->network.hostname[0] == '\0') {
        gw_hostname_from(cfg->sparkplug.edge_node_id, cfg->network.hostname,
                         sizeof(cfg->network.hostname));
    }
    /* #169: the UMH preset refuses what the lint finds as an error */
    if (ok && cfg->sparkplug.naming == GW_NAMING_UMH) {
        gw_config_lint(cfg, lint_refuse, err);
        ok = err->msg[0] == '\0';
    }
    if (!ok) {
        memset(cfg, 0, sizeof(*cfg));
    }
    return ok;
}

bool gw_config_device_id(const gw_config_t *cfg, uint16_t dev, char *out, size_t cap)
{
    const char *prefix = cfg->sparkplug.device_prefix;
    int n = prefix[0] != '\0' ? snprintf(out, cap, "%s:%s", prefix, cfg->devices[dev].name)
                              : snprintf(out, cap, "%s", cfg->devices[dev].name);
    return n >= 0 && (size_t)n < cap;
}

void gw_config_mdns_instance(const gw_config_t *cfg, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", cfg->sparkplug.group_id, cfg->sparkplug.edge_node_id);
    /* two IDs of 32 bytes give 65: cut before the character that passes the label limit */
    size_t max = GW_MDNS_INSTANCE_SIZE - 1 < cap - 1 ? GW_MDNS_INSTANCE_SIZE - 1 : cap - 1;
    size_t n = strlen(out), i = 0;
    while (i < n) {
        size_t k = gw_utf8_seq(out + i, n - i);
        if (k == 0 || i + k > max) {
            break;
        }
        i += k;
    }
    out[i] = '\0';
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
    return GW_NO_DEVICE;   /* not 0: that is a valid device and would hide the error */
}

uint16_t gw_tag_width(const gw_tag_cfg_t *tag)
{
    switch (tag->type) {
    case GW_TYPE_UINT32:
    case GW_TYPE_INT32:
    case GW_TYPE_FLOAT32:
        return 2;
    case GW_TYPE_UINT64:
    case GW_TYPE_INT64:
    case GW_TYPE_FLOAT64:
        return 4;
    case GW_TYPE_STRING:
    case GW_TYPE_BYTES:
        return tag->length;
    default:
        return 1;   /* 16-bit types, coils, discrete inputs and register bits */
    }
}

static int name_index(const char *name, const char *const *names, size_t n)
{
    for (size_t i = 0; name != NULL && i < n; i++) {
        if (strcmp(name, names[i]) == 0) {
            return (int)i;
        }
    }
    return -1;
}

bool gw_config_area_of(const char *name, gw_area_t *out)
{
    int i = name_index(name, k_area_names, sizeof(k_area_names) / sizeof(k_area_names[0]));
    if (i >= 0) {
        *out = (gw_area_t)i;
    }
    return i >= 0;
}

bool gw_config_type_of(const char *name, gw_type_t *out)
{
    int i = name_index(name, k_type_names, TYPE_COUNT);
    if (i >= 0) {
        *out = (gw_type_t)i;
    }
    return i >= 0;
}

const char *gw_config_type_name(gw_type_t type)
{
    return (size_t)type < TYPE_COUNT ? k_type_names[type] : "?";
}

bool gw_config_word_order_of(const char *name, gw_word_order_t *out)
{
    int i = name_index(name, k_word_names, sizeof(k_word_names) / sizeof(k_word_names[0]));
    if (i >= 0) {
        *out = k_word_values[i];
    }
    return i >= 0;
}
