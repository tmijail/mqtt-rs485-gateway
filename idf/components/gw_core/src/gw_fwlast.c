#include "gw_fwlast.h"

#include <stdio.h>
#include <string.h>

static const char k_codes[] = "\0stifjb";   /* index: gw_fwlast_result_t */

size_t gw_fwlast_format(const gw_fwlast_t *r, char *out, size_t cap)
{
    if (cap == 0) {
        return 0;
    }
    size_t limit = cap < GW_FWLAST_MAX ? cap : GW_FWLAST_MAX;
    char v[GW_FWLAST_VERSION];
    snprintf(v, sizeof(v), "%s", r->version);
    for (char *c = v; *c; c++) {
        if (*c == ';') {
            *c = '_';
        }
    }
    unsigned idx = (unsigned)r->result < sizeof(k_codes) - 1 ? (unsigned)r->result : 0u;
    int n = snprintf(out, limit, "r=%c;m=%c;v=%s;c=", idx ? k_codes[idx] : '-',
                     r->expect == GW_OTA_MODE_NORMAL ? 'n' : 'r', v);
    if (n < 0 || (size_t)n >= limit) {
        out[limit - 1] = '\0';
        return strlen(out);
    }
    size_t room = limit - 1 - (size_t)n;
    size_t len = strlen(r->cause);
    if (len > room) {
        len = room;
        while (len > 0 && ((unsigned char)r->cause[len] & 0xC0u) == 0x80u) {
            len--;   /* never in the middle of a UTF-8 sequence */
        }
    }
    memcpy(out + n, r->cause, len);
    out[(size_t)n + len] = '\0';
    return (size_t)n + len;
}

bool gw_fwlast_parse(const char *s, gw_fwlast_t *r)
{
    memset(r, 0, sizeof(*r));
    r->expect = GW_OTA_MODE_RECOVERY;
    const char *p = s;
    while (p != NULL && *p != '\0') {
        const char *end = strchr(p, ';');
        size_t len = end != NULL ? (size_t)(end - p) : strlen(p);
        if (len >= 2 && p[1] == '=') {
            const char *val = p + 2;
            size_t vlen = len - 2;
            switch (p[0]) {
            case 'r':
                if (vlen == 1) {
                    const char *c = strchr(k_codes + 1, val[0]);
                    r->result = c != NULL ? (gw_fwlast_result_t)(c - k_codes) : GW_FWLAST_NONE;
                }
                break;
            case 'm':
                r->expect = vlen == 1 && val[0] == 'n' ? GW_OTA_MODE_NORMAL : GW_OTA_MODE_RECOVERY;
                break;
            case 'v':
                snprintf(r->version, sizeof(r->version), "%.*s", (int)vlen, val);
                break;
            case 'c':
                snprintf(r->cause, sizeof(r->cause), "%s", val);   /* the rest of the string */
                end = NULL;
                break;
            default:
                break;   /* a key of a newer image */
            }
        }
        p = end != NULL ? end + 1 : NULL;
    }
    if (r->result == GW_FWLAST_NONE) {
        memset(r, 0, sizeof(*r));
        return false;
    }
    return true;
}

static void set(gw_fwlast_t *r, gw_fwlast_result_t result, const char *cause_fmt,
                const char *arg)
{
    r->result = result;
    if (cause_fmt != NULL) {
        snprintf(r->cause, sizeof(r->cause), "%s(%s)", cause_fmt, arg != NULL ? arg : "?");
    }
}

bool gw_fwlast_on_boot(gw_fwlast_t *r, const gw_fwlast_boot_t *b, gw_ota_mode_t *expected)
{
    bool mine = strcmp(r->version, b->running) == 0;
    bool open = r->result == GW_FWLAST_RESTARTING || r->result == GW_FWLAST_TESTING;
    if (b->pending) {
        if (mine && open) {
            *expected = r->expect;
            if (r->result == GW_FWLAST_TESTING) {
                return false;
            }
            r->result = GW_FWLAST_TESTING;
            return true;
        }
        *expected = GW_OTA_MODE_RECOVERY;
        memset(r, 0, sizeof(*r));
        r->result = GW_FWLAST_TESTING;
        r->expect = GW_OTA_MODE_RECOVERY;
        snprintf(r->version, sizeof(r->version), "%s", b->running);
        return true;
    }
    *expected = GW_OTA_MODE_NONE;
    if (mine && (open || r->result == GW_FWLAST_REJECTED)) {
        r->result = GW_FWLAST_INSTALLED;
        r->cause[0] = '\0';
        return true;
    }
    if (!mine && open) {
        if (b->other_rejected) {
            set(r, GW_FWLAST_ROLLED_BACK, "restarted during the test ", b->reset);
        } else {
            set(r, GW_FWLAST_FAILED, "interrupted before the restart ", b->reset);
        }
        return true;
    }
    if (!mine && r->result == GW_FWLAST_REJECTED) {
        r->result = GW_FWLAST_ROLLED_BACK;
        return true;
    }
    return false;
}

void gw_fwlast_state(const gw_fwlast_t *r, const char *running, char *out, size_t cap)
{
    switch (r->result) {
    case GW_FWLAST_RESTARTING:
        snprintf(out, cap, "restarting into %s", r->version);
        break;
    case GW_FWLAST_TESTING:
        snprintf(out, cap, "testing %s", r->version);
        break;
    case GW_FWLAST_INSTALLED:
        if (strcmp(r->version, running) == 0) {
            snprintf(out, cap, "installed %s", r->version);
        } else {
            snprintf(out, cap, "idle");
        }
        break;
    case GW_FWLAST_FAILED:
        snprintf(out, cap, "failed: %s", r->cause);
        break;
    case GW_FWLAST_REJECTED:
        snprintf(out, cap, "rejected %s: %s", r->version, r->cause);
        break;
    case GW_FWLAST_ROLLED_BACK:
        snprintf(out, cap, "rolled back from %s: %s", r->version, r->cause);
        break;
    default:
        snprintf(out, cap, "idle");
        break;
    }
}
