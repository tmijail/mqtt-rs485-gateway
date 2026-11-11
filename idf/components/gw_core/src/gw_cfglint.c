/*
 * #169: name checks for UMH Core and other UNS hosts (gw_config_lint).
 *
 * Sparkplug permits these names, so the validation accepts them. But UMH Core (benthos-umh
 * v0.16.2) changes them with no error, and the data goes to a wrong place in the UNS:
 *   - a location ID (sparkplug.edge_node_id, sparkplug.device_prefix, devices[].name) is split
 *     at ':' into levels; '.' also starts a level, a level that starts with '_' becomes the
 *     data contract, and each character outside [A-Za-z0-9._-] becomes '_';
 *   - a metric name (a tag name) is split at the last ':' first, then at '/': the virtual path.
 * Thus each level of a location ID must match [A-Za-z0-9][A-Za-z0-9_-]*, and each segment of a
 * tag name between '/' must match [A-Za-z0-9][A-Za-z0-9._-]* (the rules of #169). These are the
 * name findings: warnings, or errors with the UMH preset (sparkplug.naming "umh").
 *
 * Other findings stay warnings, also with the preset: sparkplug.primary_host_id (UMH publishes
 * a Sparkplug 2.x STATE, #177), a device type (UMH gives no value for a template instance),
 * and a bytes tag (UMH gives no value for Bytes).
 *
 * It runs only at the validation, on the parsed structure: no allocation, and the message
 * buffers are on the stack of the caller of the callback.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "gw_config.h"

typedef struct {
    gw_lint_cb_t cb;
    void *ctx;
    bool stopped;
    size_t count;
    gw_lint_level_t names;   /* the level of a name finding */
} lint_t;

static void report(lint_t *l, gw_lint_level_t level, const char *path, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void report(lint_t *l, gw_lint_level_t level, const char *path, const char *fmt, ...)
{
    l->count++;
    if (l->cb == NULL || l->stopped) {
        return;
    }
    char msg[GW_CFG_ERR_SIZE];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (level == GW_LINT_ERROR && n >= 0 && (size_t)n < sizeof(msg)) {
        snprintf(msg + n, sizeof(msg) - (size_t)n, " (sparkplug.naming \"umh\")");
    }
    if (!l->cb(l->ctx, level, path, msg)) {
        l->stopped = true;
    }
}

static bool alnum(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

/* The first problem of the level or segment @p s of @p n bytes, or NULL. @p metric: a segment of
 * a metric name ('.' is permitted, ':' is not), else a level of a location ID ('.' is not). */
static const char *segment_problem(const char *s, size_t n, bool metric, char *bad)
{
    if (n == 0) {
        return metric ? "is empty: UMH removes it" : "is empty: UMH removes the level";
    }
    if (s[0] == '_') {
        return metric ? "starts with '_'" : "starts with '_': UMH reads the level as the data "
                                            "contract";
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x80) {
            return "has a character that is not ASCII: UMH changes it to '_'";
        }
        if (c == ' ') {
            return "has a space: UMH changes it to '_'";
        }
        if (c == '.' && !metric) {
            return "has '.': UMH reads it as one more level";
        }
        if (c == ':') {   /* only in a metric name: a location ID is split at ':' already */
            return "has ':': UMH splits a metric name at the last ':' before it examines '/'";
        }
        if (!alnum((char)c) && c != '_' && c != '-' && c != '.') {
            *bad = (char)c;
            return "has a character that UMH changes to '_'";
        }
    }
    if (!alnum(s[0])) {
        *bad = s[0];
        return "does not start with a letter or a digit";
    }
    return NULL;
}

/* Each level of the location ID @p id (split at ':'). */
static void check_location(lint_t *l, const char *path, const char *id)
{
    unsigned k = 1;
    for (const char *s = id;; k++) {
        const char *e = strchr(s, ':');
        size_t n = e != NULL ? (size_t)(e - s) : strlen(s);
        char bad = 0;
        const char *why = segment_problem(s, n, false, &bad);
        if (why != NULL && bad != 0) {
            report(l, l->names, path, "level %u '%.*s' %s ('%c')", k, (int)n, s, why, bad);
        } else if (why != NULL) {
            report(l, l->names, path, "level %u '%.*s' %s", k, (int)n, s, why);
        }
        if (e == NULL) {
            break;
        }
        s = e + 1;
    }
}

/* Each segment of the metric name @p name (split at '/'). */
static void check_metric(lint_t *l, const char *path, const char *name)
{
    unsigned k = 1;
    for (const char *s = name;; k++) {
        const char *e = strchr(s, '/');
        size_t n = e != NULL ? (size_t)(e - s) : strlen(s);
        char bad = 0;
        const char *why = segment_problem(s, n, true, &bad);
        if (why != NULL && bad != 0) {
            report(l, l->names, path, "segment %u '%.*s' of the metric name %s ('%c')", k, (int)n,
                   s, why, bad);
        } else if (why != NULL) {
            report(l, l->names, path, "segment %u '%.*s' of the metric name %s", k, (int)n, s,
                   why);
        }
        if (e == NULL) {
            break;
        }
        s = e + 1;
    }
}

size_t gw_config_lint(const gw_config_t *cfg, gw_lint_cb_t cb, void *ctx)
{
    bool umh = cfg->sparkplug.naming == GW_NAMING_UMH;
    lint_t l = {.cb = cb, .ctx = ctx, .names = umh ? GW_LINT_ERROR : GW_LINT_WARNING};
    char path[GW_CFG_PATH_SIZE];

    check_location(&l, "sparkplug.edge_node_id", cfg->sparkplug.edge_node_id);
    if (cfg->sparkplug.device_prefix[0] != '\0') {
        check_location(&l, "sparkplug.device_prefix", cfg->sparkplug.device_prefix);
    }
    if (cfg->sparkplug.primary_host_id[0] != '\0') {
        report(&l, GW_LINT_WARNING, "sparkplug.primary_host_id",
               "UMH Core cannot be the primary host: it publishes a Sparkplug 2.x STATE (#177). "
               "Keep it only for a Sparkplug 3.0 host");
    }
    for (uint16_t i = 0; i < cfg->device_count; i++) {
        const gw_device_cfg_t *d = &cfg->devices[i];
        snprintf(path, sizeof(path), "devices[%u].name", (unsigned)i);
        check_location(&l, path, d->name);
        if (d->type[0] != '\0') {
            snprintf(path, sizeof(path), "devices[%u].type", (unsigned)i);
            report(&l, GW_LINT_WARNING, path,
                   "UMH Core gives no value for a template instance: the tags of the device do "
                   "not get to the UNS");
        }
        for (uint16_t k = 0; k < d->tag_count; k++) {
            const gw_tag_cfg_t *t = &cfg->tags[d->first_tag + k];
            snprintf(path, sizeof(path), "devices[%u].tags[%u].name", (unsigned)i, (unsigned)k);
            check_metric(&l, path, t->name);
            if (t->type == GW_TYPE_BYTES) {
                snprintf(path, sizeof(path), "devices[%u].tags[%u].type", (unsigned)i,
                         (unsigned)k);
                report(&l, GW_LINT_WARNING, path,
                       "UMH Core gives no value for a Bytes metric: the tag does not get to the "
                       "UNS");
            }
        }
    }
    return l.count;
}
