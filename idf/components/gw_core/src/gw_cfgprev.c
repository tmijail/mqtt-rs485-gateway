#include "gw_cfgprev.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "gw_config_priv.h"
#include "gw_port.h"

/* ---------- the NVS record ---------- */

static void clean_version(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src);
    for (char *c = dst; *c; c++) {
        if (*c == ';') {
            *c = '_';
        }
    }
}

size_t gw_cfgprev_format(const gw_cfgprev_t *p, char *out, size_t cap)
{
    if (cap == 0) {
        return 0;
    }
    size_t limit = cap < GW_CFGPREV_MAX ? cap : GW_CFGPREV_MAX;
    char a[GW_CFGPREV_VERSION], v[GW_CFGPREV_VERSION];
    clean_version(a, sizeof(a), p->armed);
    clean_version(v, sizeof(v), p->version);
    int n;
    if (v[0] == '\0') {
        n = snprintf(out, limit, "a=%s", a);
    } else {
        n = snprintf(out, limit, "a=%s;v=%s;s=%u;t=%" PRId64 ";h=%08" PRIx32, a, v,
                     (unsigned)p->schema, p->saved, p->hash);
    }
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n < limit ? (size_t)n : limit - 1;
}

bool gw_cfgprev_parse(const char *s, gw_cfgprev_t *p)
{
    memset(p, 0, sizeof(*p));
    bool have_hash = false;
    for (const char *q = s; q != NULL && *q != '\0';) {
        const char *end = strchr(q, ';');
        size_t len = end != NULL ? (size_t)(end - q) : strlen(q);
        if (len >= 2 && q[1] == '=') {
            const char *val = q + 2;
            int vlen = (int)(len - 2);
            char num[24];
            snprintf(num, sizeof(num), "%.*s", vlen, val);
            switch (q[0]) {
            case 'a':
                snprintf(p->armed, sizeof(p->armed), "%.*s", vlen, val);
                break;
            case 'v':
                snprintf(p->version, sizeof(p->version), "%.*s", vlen, val);
                break;
            case 's':
                p->schema = (uint16_t)strtoul(num, NULL, 10);
                break;
            case 't':
                p->saved = (int64_t)strtoll(num, NULL, 10);
                break;
            case 'h':
                p->hash = (uint32_t)strtoul(num, NULL, 16);
                have_hash = vlen > 0;
                break;
            default:
                break;   /* a key of a newer image */
            }
        }
        q = end != NULL ? end + 1 : NULL;
    }
    if (!have_hash) {
        p->version[0] = '\0';   /* a capture without its hash cannot be compared */
    }
    if (p->armed[0] == '\0' && p->version[0] == '\0') {
        memset(p, 0, sizeof(*p));
        return false;
    }
    return true;
}

uint32_t gw_cfgprev_hash(uint32_t h, const void *data, size_t len)
{
    const unsigned char *b = data;
    for (size_t i = 0; i < len; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}

bool gw_cfgprev_arm(gw_cfgprev_t *p, const char *other, const char *running)
{
    if (other == NULL || other[0] == '\0' || (running != NULL && strcmp(other, running) == 0)) {
        return false;
    }
    char v[GW_CFGPREV_VERSION];
    snprintf(v, sizeof(v), "%s", other);
    if (strcmp(p->armed, v) == 0) {
        return false;
    }
    memcpy(p->armed, v, sizeof(v));
    return true;
}

gw_cfgprev_save_t gw_cfgprev_on_save(gw_cfgprev_t *p, bool old_valid, uint16_t old_schema,
                                     uint32_t old_hash, uint32_t new_hash, int64_t now)
{
    if (p->armed[0] == '\0') {
        return GW_CFGPREV_KEEP;
    }
    if (!old_valid) {
        p->armed[0] = '\0';
        return GW_CFGPREV_DISARM;
    }
    if (old_hash == new_hash) {
        return GW_CFGPREV_KEEP;   /* the same document again: wait for a change */
    }
    memcpy(p->version, p->armed, sizeof(p->version));
    p->armed[0] = '\0';
    p->schema = old_schema;
    p->saved = now >= GW_CFGPREV_TIME_MIN ? now : 0;
    p->hash = new_hash;
    return GW_CFGPREV_CAPTURE;
}

gw_cfgprev_action_t gw_cfgprev_decide(const gw_cfgprev_t *p, const char *version,
                                      uint16_t schema, uint32_t hash)
{
    if (p == NULL || p->version[0] == '\0' || version == NULL ||
        strncmp(p->version, version, sizeof(p->version) - 1) != 0 || p->schema > schema) {
        return GW_CFGPREV_NONE;
    }
    return p->hash == hash ? GW_CFGPREV_RESTORE : GW_CFGPREV_CHANGED;
}

/* ---------- the paths that differ ---------- */

#define DIFF_LIST 3     /* paths named in the text */
#define DIFF_PATH 96    /* a longer path is cut */
#define DIFF_DEPTH 32   /* deeper levels count as one difference */

typedef struct {
    char *out;
    size_t cap;
    size_t used;
    size_t count;
} diff_t;

/* The secrets of the schema (gw_cfgdoc.h): NVS holds them, not the documents. A saved document
 * has none; a document written by hand can have one, also "" (no secret). Their keys are no
 * difference between two documents. */
static const char *const k_secret_paths[] = {"mqtt.password", "network.wifi.password",
                                             "web.password"};

static void found(diff_t *d, const char *path)
{
    for (size_t i = 0; i < sizeof(k_secret_paths) / sizeof(k_secret_paths[0]); i++) {
        if (strcmp(path, k_secret_paths[i]) == 0) {
            return;
        }
    }
    if (d->count < DIFF_LIST && d->used < d->cap) {
        int n = snprintf(d->out + d->used, d->cap - d->used, "%s%s", d->count ? ", " : "",
                         path[0] ? path : "(the whole document)");
        if (n > 0) {
            d->used += (size_t)n < d->cap - d->used ? (size_t)n : d->cap - d->used - 1;
        }
    }
    d->count++;
}

static bool same_leaf(const cJSON *a, const cJSON *b)
{
    if ((a->type & 0xFF) != (b->type & 0xFF)) {
        return false;
    }
    if (cJSON_IsString(a)) {
        return strcmp(a->valuestring, b->valuestring) == 0;
    }
    if (cJSON_IsNumber(a)) {
        return a->valuedouble == b->valuedouble;
    }
    return true;   /* true, false, null */
}

static void walk(diff_t *d, const cJSON *a, const cJSON *b, char *path, size_t at, int depth);

static void child(diff_t *d, const cJSON *a, const cJSON *b, char *path, size_t at, int depth,
                  const char *key, int index)
{
    int n = key != NULL ? snprintf(path + at, DIFF_PATH - at, "%s%s", at ? "." : "", key)
                        : snprintf(path + at, DIFF_PATH - at, "[%d]", index);
    size_t next = n > 0 && (size_t)n < DIFF_PATH - at ? at + (size_t)n : DIFF_PATH - 1;
    if (a == NULL || b == NULL) {
        found(d, path);
    } else {
        walk(d, a, b, path, next, depth + 1);
    }
    path[at] = '\0';
}

static void walk(diff_t *d, const cJSON *a, const cJSON *b, char *path, size_t at, int depth)
{
    bool obj = cJSON_IsObject(a) && cJSON_IsObject(b);
    bool arr = cJSON_IsArray(a) && cJSON_IsArray(b);
    if ((!obj && !arr) || depth >= DIFF_DEPTH) {
        if (!same_leaf(a, b) || ((obj || arr) && !cJSON_Compare(a, b, true))) {
            found(d, path);
        }
        return;
    }
    if (obj) {
        for (const cJSON *x = a->child; x != NULL; x = x->next) {
            child(d, x, cJSON_GetObjectItemCaseSensitive(b, x->string), path, at, depth,
                  x->string, 0);
        }
        for (const cJSON *y = b->child; y != NULL; y = y->next) {
            if (cJSON_GetObjectItemCaseSensitive(a, y->string) == NULL) {
                child(d, NULL, y, path, at, depth, y->string, 0);
            }
        }
        return;
    }
    const cJSON *x = a->child, *y = b->child;
    for (int i = 0; x != NULL || y != NULL; i++) {
        child(d, x, y, path, at, depth, NULL, i);
        x = x != NULL ? x->next : NULL;
        y = y != NULL ? y->next : NULL;
    }
}

size_t gw_cfgprev_diff(const char *a, size_t alen, const char *b, size_t blen, char *out,
                       size_t cap)
{
    if (cap == 0) {
        return (size_t)-1;
    }
    out[0] = '\0';
    gw_port_json_lock();
    gw_config_json_hooks();
    cJSON *ja = cJSON_ParseWithLength(a, alen);
    cJSON *jb = ja != NULL ? cJSON_ParseWithLength(b, blen) : NULL;
    size_t count = (size_t)-1;
    if (ja == NULL || jb == NULL) {
        snprintf(out, cap, "%s", gw_config_json_alloc_failed() ? "no memory to compare them"
                                                                : "a document does not parse");
    } else {
        char path[DIFF_PATH] = "";
        diff_t d = {.out = out, .cap = cap};
        walk(&d, ja, jb, path, 0, 0);
        if (d.count > DIFF_LIST && d.used < cap) {
            snprintf(out + d.used, cap - d.used, " and %u more", (unsigned)(d.count - DIFF_LIST));
        }
        count = d.count;
    }
    cJSON_Delete(ja);
    cJSON_Delete(jb);
    gw_port_json_unlock();
    return count;
}

/* ---------- the text of the web UI ---------- */

/* Civil date of a day count since 1970-01-01 (H. Hinnant's algorithm). */
static void civil(int64_t days, int *y, unsigned *m, unsigned *d)
{
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)((int64_t)yoe + era * 400 + (*m <= 2 ? 1 : 0));
}

void gw_cfgprev_text(const gw_cfgprev_t *p, char *out, size_t cap)
{
    if (cap == 0) {
        return;
    }
    if (p->version[0] == '\0') {
        out[0] = '\0';
        return;
    }
    if (p->saved < GW_CFGPREV_TIME_MIN) {
        snprintf(out, cap, "configuration from before the update: %s, saved at an unknown time",
                 p->version);
        return;
    }
    int y;
    unsigned m, d;
    civil(p->saved / 86400, &y, &m, &d);
    unsigned s = (unsigned)(p->saved % 86400);
    snprintf(out, cap, "configuration from before the update: %s, saved on %04d-%02u-%02u "
             "%02u:%02u UTC", p->version, y, m, d, s / 3600, s / 60 % 60);
}
