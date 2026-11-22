#include "gw_access.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char *out;
    size_t cap;
    size_t len;   /* the length of the full text, also past cap */
} sink_t;

static void put(sink_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void put(sink_t *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t room = s->len < s->cap ? s->cap - s->len : 0;
    int n = vsnprintf(room > 0 ? s->out + s->len : NULL, room, fmt, ap);
    va_end(ap);
    if (n > 0) {
        s->len += (size_t)n;
    }
}

/* A value from the configuration: no control character goes out (it could make a new line). */
static const char *clean(const char *in, char *buf, size_t cap)
{
    size_t i = 0;
    for (; in != NULL && in[i] != '\0' && i + 1 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        buf[i] = c < 0x20 || c == 0x7F ? '?' : (char)c;
    }
    buf[i] = '\0';
    return buf;
}

static bool has(const char *s)
{
    return s != NULL && s[0] != '\0';
}

size_t gw_access_format(const gw_access_t *a, char *out, size_t cap)
{
    sink_t s = {.out = out, .cap = cap, .len = 0};
    char v[128];
    if (cap > 0) {
        out[0] = '\0';
    }
    put(&s, "access-data v%d\n", GW_ACCESS_VERSION);
    put(&s, "mode %s\n", has(a->mode) ? a->mode : "?");
    put(&s, "firmware %s\n", clean(a->firmware, v, sizeof(v)));
    if (has(a->hostname)) {
        put(&s, "hostname %s\n", clean(a->hostname, v, sizeof(v)));
    }
    const char *scheme = a->https ? "https" : "http";
    if (a->web_running) {
        put(&s, "web on %s %u\n", scheme, (unsigned)a->port);
    } else {
        put(&s, "web off\n");
    }
    if (!a->password_set) {
        put(&s, "login commissioning\nuser admin\n");
        if (has(a->commissioning_password)) {
            put(&s, "commissioning-password %s\n", clean(a->commissioning_password, v, sizeof(v)));
        }
    } else if (has(a->username)) {
        put(&s, "login web-password\nuser %s\n", clean(a->username, v, sizeof(v)));
    } else {
        put(&s, "login web-password-any-user\n");
    }
    if (a->web_running && a->https && has(a->fingerprint)) {
        put(&s, "fingerprint %s\n", clean(a->fingerprint, v, sizeof(v)));
    }
    size_t n = a->n_ifs < GW_ACCESS_IF_MAX ? a->n_ifs : GW_ACCESS_IF_MAX;
    bool any_up = false, mdns_up = false;
    for (size_t i = 0; i < n; i++) {
        const gw_access_if_t *f = &a->ifs[i];
        switch (f->state) {
        case GW_ACCESS_IF_UP:
            put(&s, "if %s up %u.%u.%u.%u\n", f->name, f->ip[0], f->ip[1], f->ip[2], f->ip[3]);
            any_up = true;
            mdns_up = mdns_up || !f->no_mdns;
            break;
        case GW_ACCESS_IF_NO_ADDRESS: put(&s, "if %s no-address\n", f->name); break;
        case GW_ACCESS_IF_NO_LINK: put(&s, "if %s no-link\n", f->name); break;
        case GW_ACCESS_IF_OFF:
        default: put(&s, "if %s off\n", f->name); break;
        }
    }
    char sfx[8] = "";
    if (a->port != (a->https ? 443u : 80u)) {
        snprintf(sfx, sizeof(sfx), ":%u", (unsigned)a->port);
    }
    if (!a->web_running) {
        put(&s, "url-none web-off\n");
    } else if (!any_up) {
        put(&s, "url-none no-address\n");
    } else {
        for (size_t i = 0; i < n; i++) {
            const gw_access_if_t *f = &a->ifs[i];
            if (f->state == GW_ACCESS_IF_UP) {
                put(&s, "url %s://%u.%u.%u.%u%s/\n", scheme, f->ip[0], f->ip[1], f->ip[2], f->ip[3],
                    sfx);
            }
        }
        if (a->mdns && mdns_up && has(a->hostname)) {
            put(&s, "url %s://%s.local%s/\n", scheme, clean(a->hostname, v, sizeof(v)), sfx);
        }
    }
    put(&s, "access-data end\n");
    return s.len;
}
