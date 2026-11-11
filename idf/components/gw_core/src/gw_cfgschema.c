#include "gw_cfgschema.h"

#include <stdio.h>
#include <string.h>

void gw_schema_init(gw_schema_report_t *r, const gw_since_t *table, size_t n)
{
    memset(r, 0, sizeof(*r));
    r->table = table;
    r->n = n;
}

bool gw_schema_path_match(const char *pattern, const char *path)
{
    while (*pattern != '\0' && *path != '\0') {
        if (pattern[0] == '[' && pattern[1] == ']' && path[0] == '[') {
            const char *p = path + 1;
            while (*p >= '0' && *p <= '9') {
                p++;
            }
            if (p == path + 1 || *p != ']') {
                return false;
            }
            pattern += 2;
            path = p + 1;
            continue;
        }
        if (*pattern != *path) {
            return false;
        }
        pattern++;
        path++;
    }
    return *pattern == '\0' && *path == '\0';
}

uint16_t gw_schema_since(const gw_since_t *table, size_t n, const char *path, const char *value)
{
    for (size_t i = 0; i < n; i++) {
        const gw_since_t *e = &table[i];
        bool same_kind = (e->value == NULL) == (value == NULL);
        if (same_kind && (value == NULL || strcmp(e->value, value) == 0) &&
            gw_schema_path_match(e->path, path)) {
            return e->since;
        }
    }
    return 1;
}

void gw_schema_note(gw_schema_report_t *r, const char *path, const char *value)
{
    uint16_t since = gw_schema_since(r->table, r->n, path, value);
    r->count++;
    if (since > r->need) {
        r->need = since;
    }
    r->hist[since < GW_SCHEMA_HIST ? since : GW_SCHEMA_HIST - 1]++;
    /* keep the highest numbers, the first one seen among equal numbers */
    size_t at = GW_SCHEMA_TOP;
    for (size_t i = 0; i < GW_SCHEMA_TOP; i++) {
        if (since > r->top[i].since) {
            at = i;
            break;
        }
    }
    if (at == GW_SCHEMA_TOP) {
        return;
    }
    memmove(&r->top[at + 1], &r->top[at], (GW_SCHEMA_TOP - 1 - at) * sizeof(r->top[0]));
    r->top[at].since = since;
    if (value != NULL) {
        snprintf(r->top[at].path, sizeof(r->top[at].path), "%s=%s", path, value);
    } else {
        snprintf(r->top[at].path, sizeof(r->top[at].path), "%s", path);
    }
}

uint32_t gw_schema_newer(const gw_schema_report_t *r, uint16_t schema)
{
    uint32_t n = 0;
    for (size_t s = (size_t)schema + 1; s < GW_SCHEMA_HIST; s++) {
        n += r->hist[s];
    }
    if (schema >= GW_SCHEMA_HIST - 1 && r->need > schema) {
        /* the shared last count cannot tell: the listed paths at least */
        for (size_t i = 0; i < GW_SCHEMA_TOP; i++) {
            n += r->top[i].since > schema ? 1u : 0u;
        }
    }
    return n;
}

bool gw_schema_fits(const gw_schema_report_t *r, uint16_t schema, char *why, size_t cap)
{
    if (r->need <= schema) {
        return true;
    }
    uint32_t newer = gw_schema_newer(r, schema);
    int n = snprintf(why, cap, "the configuration needs schema %u, the image reads %u:",
                     (unsigned)r->need, (unsigned)schema);
    uint32_t listed = 0;
    for (size_t i = 0; i < GW_SCHEMA_TOP && n >= 0 && (size_t)n < cap; i++) {
        if (r->top[i].since <= schema) {
            break;
        }
        n += snprintf(why + n, cap - (size_t)n, "%s %s (%u)", listed ? "," : "", r->top[i].path,
                      (unsigned)r->top[i].since);
        listed++;
    }
    if (newer > listed && n >= 0 && (size_t)n < cap) {
        snprintf(why + n, cap - (size_t)n, " and %lu more", (unsigned long)(newer - listed));
    }
    return false;
}
