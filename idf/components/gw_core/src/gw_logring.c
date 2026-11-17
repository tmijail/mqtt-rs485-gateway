#include "gw_logring.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "gw_config_priv.h"
#include "gw_port.h"

void gw_logring_init(gw_logring_t *r, char *buf, size_t size)
{
    memset(r, 0, sizeof(*r));
    r->buf = buf;
    r->size = size;
    r->next = 1;
}

uint32_t gw_logring_first(const gw_logring_t *r)
{
    return r->next - r->lines;
}

/* Remove the oldest line. */
static void drop_oldest(gw_logring_t *r)
{
    size_t n = 0;
    while (n < r->used && r->buf[(r->head + n) % r->size] != '\n') {
        n++;
    }
    n = n < r->used ? n + 1 : r->used;   /* with its '\n' */
    r->head = (r->head + n) % r->size;
    r->used -= n;
    r->lines--;
}

static void put_byte(gw_logring_t *r, char c)
{
    r->buf[(r->head + r->used) % r->size] = c;
    r->used++;
}

/* The bytes of @p line without its ANSI color sequences, its control characters (spaces) and
 * its '\r' and '\n' at the end, at most @p max: written into @p r when it is not NULL. Returns
 * the count. */
static size_t clean(const char *line, size_t len, gw_logring_t *r, size_t max)
{
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        len--;
    }
    size_t n = 0;
    for (size_t i = 0; i < len && n < max; i++) {
        unsigned char c = (unsigned char)line[i];
        if (c == 0x1b && i + 1 < len && line[i + 1] == '[') {   /* ESC [ ... final byte */
            i += 2;
            while (i < len && !((unsigned char)line[i] >= 0x40 && (unsigned char)line[i] <= 0x7e)) {
                i++;
            }
            continue;
        }
        if (r != NULL) {
            put_byte(r, c < 0x20 || c == 0x7f ? ' ' : (char)c);
        }
        n++;
    }
    return n;
}

void gw_logring_put(gw_logring_t *r, const char *line, size_t len)
{
    size_t max = r->size > GW_LOGRING_LINE_MAX ? GW_LOGRING_LINE_MAX : r->size - 1;
    size_t n = r->size < 2 ? 0 : clean(line, len, NULL, max);
    if (n == 0) {
        return;
    }
    while (r->used + n + 1 > r->size) {
        drop_oldest(r);
    }
    clean(line, len, r, n);
    put_byte(r, '\n');
    r->lines++;
    r->next++;
}

size_t gw_logring_copy(const gw_logring_t *r, uint32_t since, char *out, size_t cap,
                       uint32_t *from, uint32_t *last)
{
    *from = *last = since;
    uint32_t seq = gw_logring_first(r);
    size_t off = 0, n = 0;
    bool first = true;
    while (off < r->used) {
        size_t len = 0;   /* this line with its '\n' */
        while (off + len < r->used && r->buf[(r->head + off + len) % r->size] != '\n') {
            len++;
        }
        len++;
        if ((int32_t)(seq - since) > 0) {
            if (n + len > cap) {
                break;
            }
            for (size_t k = 0; k < len; k++) {
                out[n + k] = r->buf[(r->head + off + k) % r->size];
            }
            n += len;
            if (first) {
                *from = seq;
                first = false;
            }
            *last = seq;
        }
        off += len;
        seq++;
    }
    return n;
}

bool gw_logring_tag(const char *line, size_t len, const char **tag, size_t *tag_len)
{
    size_t i = 0;
    if (len >= 2 && line[0] == 0x1b && line[1] == '[') {   /* the color before the level */
        while (i < len && line[i] != 'm') {
            i++;
        }
        i++;
    }
    if (i + 3 > len || strchr("EWIDV", line[i]) == NULL || line[i + 1] != ' ' || line[i + 2] != '(') {
        return false;
    }
    i += 3;
    while (i < len && line[i] != ')') {
        i++;
    }
    if (i + 2 >= len || line[i + 1] != ' ') {
        return false;
    }
    size_t start = i + 2, end = start;
    while (end < len && line[end] != ':' && line[end] != ' ') {
        end++;
    }
    if (end == start || end >= len || line[end] != ':') {
        return false;
    }
    *tag = line + start;
    *tag_len = end - start;
    return true;
}

/* ---- levels ---- */

static const char *const k_level_names[] = {"none", "error", "warn", "info", "debug", "verbose"};

const char *gw_loglevel_name(int level)
{
    return level >= 0 && level <= GW_LOGLV_VERBOSE ? k_level_names[level] : "?";
}

int gw_loglevel_parse(const char *name)
{
    for (int i = 0; i <= GW_LOGLV_VERBOSE; i++) {
        if (strcmp(name, k_level_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

/* ---- the tag table ---- */

void gw_logtags_init(gw_logtags_t *t)
{
    memset(t, 0, sizeof(*t));
}

static gw_logtag_t *find(gw_logtags_t *t, const char *name, size_t len)
{
    for (unsigned i = 0; i < t->count; i++) {
        if (strncmp(t->tag[i].name, name, len) == 0 && t->tag[i].name[len] == '\0') {
            return &t->tag[i];
        }
    }
    return NULL;
}

gw_logtag_t *gw_logtags_add(gw_logtags_t *t, const char *name, size_t len)
{
    if (len == 0 || len >= GW_LOGTAG_SIZE) {
        return NULL;
    }
    gw_logtag_t *e = find(t, name, len);
    if (e != NULL || t->count >= GW_LOGTAG_MAX) {
        return e;
    }
    e = &t->tag[t->count++];
    memcpy(e->name, name, len);
    e->name[len] = '\0';
    e->level = GW_LOGTAG_DEFAULT;
    return e;
}

bool gw_logtags_set(gw_logtags_t *t, const char *name, int level, int build_default)
{
    gw_logtag_t *e = gw_logtags_add(t, name, strlen(name));
    if (e == NULL) {
        return false;
    }
    e->level = (int8_t)(level == build_default ? GW_LOGTAG_DEFAULT : level);
    return true;
}

void gw_logtags_reset_all(gw_logtags_t *t)
{
    for (unsigned i = 0; i < t->count; i++) {
        t->tag[i].level = GW_LOGTAG_DEFAULT;
    }
}

size_t gw_logtags_changed(const gw_logtags_t *t, const gw_logtag_t **out, size_t cap)
{
    size_t n = 0;
    for (unsigned i = 0; i < t->count && n < cap; i++) {
        if (t->tag[i].level != GW_LOGTAG_DEFAULT) {
            out[n++] = &t->tag[i];
        }
    }
    return n;
}

size_t gw_logtags_json(const gw_logtags_t *t, bool changed_only, char *out, size_t cap)
{
    size_t n = 0;
    if (cap < 3) {
        if (cap > 0) {
            out[0] = '\0';
        }
        return 0;
    }
    out[n++] = '[';
    bool first = true;
    for (unsigned i = 0; i < t->count; i++) {
        const gw_logtag_t *e = &t->tag[i];
        if (changed_only && e->level == GW_LOGTAG_DEFAULT) {
            continue;
        }
        char item[GW_LOGTAG_SIZE + 40];
        int k = changed_only ? snprintf(item, sizeof(item), "%s{\"tag\":\"%s\",\"level\":\"%s\"}",
                                        first ? "" : ",", e->name, gw_loglevel_name(e->level))
                             : snprintf(item, sizeof(item), "%s\"%s\"", first ? "" : ",", e->name);
        if (k < 0 || n + (size_t)k + 2 > cap) {
            break;   /* whole entries only, and room for "]" and the NUL */
        }
        memcpy(out + n, item, (size_t)k);
        n += (size_t)k;
        first = false;
    }
    out[n++] = ']';
    out[n] = '\0';
    return n;
}

bool gw_logtag_valid(const char *tag)
{
    size_t n = strlen(tag);
    if (n == 0 || n >= GW_LOGTAG_SIZE) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)tag[i];
        if (c <= ' ' || c >= 0x7f || c == ':' || c == '"' || c == '\\') {
            return false;
        }
    }
    return true;
}

static bool parse_set(const cJSON *root, int max_level, char *tag, int *level, bool *reset,
                      char *why, size_t cap)
{
    if (!cJSON_IsObject(root)) {
        snprintf(why, cap, "expected a JSON object");
        return false;
    }
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "reset");
    if (r != NULL) {
        if (!cJSON_IsTrue(r) || cJSON_GetArraySize(root) != 1) {
            snprintf(why, cap, "reset: expected {\"reset\": true} alone");
            return false;
        }
        *reset = true;
        return true;
    }
    *reset = false;
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "tag");
    const cJSON *l = cJSON_GetObjectItemCaseSensitive(root, "level");
    if (cJSON_GetArraySize(root) != 2 || !cJSON_IsString(t) || !cJSON_IsString(l)) {
        snprintf(why, cap, "expected {\"tag\": \"<tag or *>\", \"level\": \"<level>\"} or "
                           "{\"reset\": true}");
        return false;
    }
    if (!gw_logtag_valid(t->valuestring)) {
        snprintf(why, cap, "tag: 1 to %d characters without spaces and ':', or *",
                 GW_LOGTAG_SIZE - 1);
        return false;
    }
    int lv = gw_loglevel_parse(l->valuestring);
    if (lv < 0 || lv > max_level) {
        char list[64] = "";
        size_t n = 0;
        for (int i = 0; i <= max_level && i <= GW_LOGLV_VERBOSE && n < sizeof(list); i++) {
            int k = snprintf(list + n, sizeof(list) - n, "%s%s", i ? ", " : "",
                             gw_loglevel_name(i));
            n += k > 0 ? (size_t)k : 0;
        }
        snprintf(why, cap, "level: one of %s", list);
        return false;
    }
    snprintf(tag, GW_LOGTAG_SIZE, "%s", t->valuestring);
    *level = lv;
    return true;
}

bool gw_logring_parse_set(const char *json, size_t len, int max_level, char *tag, int *level,
                          bool *reset, char *why, size_t cap)
{
    gw_port_json_lock();
    gw_config_json_hooks();
    cJSON *root = cJSON_ParseWithLength(json, len);
    bool ok;
    if (root == NULL) {
        snprintf(why, cap, gw_config_json_alloc_failed() ? "out of memory parsing the request"
                                                         : "the request is not valid JSON");
        ok = false;
    } else {
        ok = parse_set(root, max_level, tag, level, reset, why, cap);
    }
    cJSON_Delete(root);
    gw_port_json_unlock();
    return ok;
}
