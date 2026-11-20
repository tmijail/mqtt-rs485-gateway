#include "gw_captive.h"

#include <string.h>
#include <strings.h>

/* ---------- DNS ---------- */

#define DNS_HDR       12u
#define DNS_NAME_MAX  255u   /* the wire form of a name, with its root label */
#define DNS_LABEL_MAX 63u
#define DNS_TYPE_A    1u
#define DNS_CLASS_IN  1u

static unsigned rd16(const uint8_t *p)
{
    return ((unsigned)p[0] << 8) | p[1];
}

static void wr16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

size_t gw_captive_dns_answer(const uint8_t *q, size_t len, const uint8_t ip[4], uint8_t *out,
                             size_t cap)
{
    if (q == NULL || ip == NULL || out == NULL || len < DNS_HDR ||
        len > GW_CAPTIVE_DNS_QUERY_MAX) {
        return 0;
    }
    /* byte 2: QR, OPCODE (4 bits), AA, TC, RD */
    if ((q[2] & 0x80u) != 0 || ((q[2] >> 3) & 0x0Fu) != 0 || rd16(&q[4]) != 1) {
        return 0;   /* a response, not a standard query, or not one question */
    }
    size_t p = DNS_HDR, name = 0;
    for (;;) {
        if (p >= len) {
            return 0;
        }
        size_t l = q[p];
        name += l + 1u;
        if (name > DNS_NAME_MAX) {
            return 0;
        }
        if (l == 0) {
            p++;
            break;
        }
        if (l > DNS_LABEL_MAX) {
            return 0;   /* a compression pointer (0xC0) or a reserved label type (0x40, 0x80) */
        }
        p += l + 1u;
    }
    if (len - p < 4u) {
        return 0;
    }
    bool a = rd16(&q[p]) == DNS_TYPE_A && rd16(&q[p + 2]) == DNS_CLASS_IN;
    p += 4u;   /* the end of the question */
    size_t n = p + (a ? GW_CAPTIVE_DNS_A_RR : 0u);
    if (n > cap) {
        return 0;
    }
    memcpy(out, q, p);
    out[2] = (uint8_t)(0x80u | 0x04u | (q[2] & 0x01u));   /* QR, AA, the RD of the query */
    out[3] = 0;                                           /* RA 0, Z 0, RCODE NOERROR */
    wr16(&out[4], 1);                                     /* QDCOUNT */
    wr16(&out[6], a ? 1u : 0u);                           /* ANCOUNT */
    wr16(&out[8], 0);                                     /* NSCOUNT */
    wr16(&out[10], 0);                                    /* ARCOUNT */
    if (a) {
        uint8_t *r = &out[p];
        wr16(&r[0], 0xC000u | DNS_HDR);   /* the name of the question */
        wr16(&r[2], DNS_TYPE_A);
        wr16(&r[4], DNS_CLASS_IN);
        wr16(&r[6], 0);
        wr16(&r[8], GW_CAPTIVE_DNS_TTL_S);
        wr16(&r[10], 4);
        memcpy(&r[12], ip, 4);
    }
    return n;
}

/* ---------- HTTP ---------- */

/* The length of the path of @p uri: up to '?' or '#'. */
static size_t path_len(const char *uri)
{
    return strcspn(uri, "?#");
}

bool gw_captive_probe_path(const char *uri)
{
    static const char *const probes[] = {
        "/generate_204", "/gen_204",                        /* Android, Chrome */
        "/hotspot-detect.html", "/library/test/success.html",   /* iOS, macOS */
        "/ncsi.txt", "/connecttest.txt", "/redirect",       /* Windows */
        "/canonical.html", "/success.txt",                  /* Firefox */
        "/check_network_status.txt",                        /* GNOME NetworkManager */
        "/kindle-wifi/wifistub.html",                       /* Kindle */
    };
    if (uri == NULL) {
        return false;
    }
    size_t n = path_len(uri);
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        if (strlen(probes[i]) == n && strncasecmp(uri, probes[i], n) == 0) {
            return true;
        }
    }
    return false;
}

/* @p a (@p n bytes) is @p b, case not significant. */
static bool same(const char *a, size_t n, const char *b)
{
    return b != NULL && b[0] != '\0' && strlen(b) == n && strncasecmp(a, b, n) == 0;
}

bool gw_captive_host_is_gateway(const char *host, const char *local_ip, const char *hostname)
{
    if (host == NULL) {
        return false;
    }
    size_t n = strlen(host);
    if (n == 0) {
        return true;   /* HTTP/1.0: no Host */
    }
    if (host[0] == '[') {
        return false;   /* an IPv6 literal: the access point has IPv4 only */
    }
    const char *colon = memchr(host, ':', n);
    if (colon != NULL) {
        n = (size_t)(colon - host);
    }
    if (n > 0 && host[n - 1] == '.') {
        n--;   /* a fully qualified name: "gw01.local." */
    }
    if (n == 0) {
        return false;
    }
    if (same(host, n, local_ip) || same(host, n, hostname)) {
        return true;
    }
    static const char local_sfx[] = ".local";
    size_t sl = sizeof(local_sfx) - 1u;
    return n > sl && strncasecmp(host + n - sl, local_sfx, sl) == 0 &&
           same(host, n - sl, hostname);
}

gw_captive_route_t gw_captive_route(bool from_ap, bool get, const char *uri, const char *host,
                                    const char *local_ip, const char *hostname)
{
    if (!from_ap || !get || uri == NULL) {
        return GW_CAPTIVE_NONE;
    }
    size_t n = path_len(uri);
    if (n == sizeof(GW_CAPTIVE_PATH) - 1u && strncmp(uri, GW_CAPTIVE_PATH, n) == 0) {
        return GW_CAPTIVE_PAGE;
    }
    if (gw_captive_probe_path(uri) || !gw_captive_host_is_gateway(host, local_ip, hostname)) {
        return GW_CAPTIVE_REDIRECT;
    }
    return GW_CAPTIVE_NONE;
}

/* ---------- the guide page ---------- */

/* The value of tag @p t (@p n bytes); false for an unknown name. */
static bool value_of(const gw_captive_page_t *v, const char *t, size_t n, const char **out)
{
    if (same(t, n, "node")) {
        *out = v->node;
    } else if (same(t, n, "url")) {
        *out = v->url;
    } else if (same(t, n, "mdns")) {
        *out = v->mdns;
    } else if (same(t, n, "ssid")) {
        *out = v->ssid;
    } else {
        return false;
    }
    if (*out == NULL) {
        *out = "";
    }
    return true;
}

/* The condition of section @p t (@p n bytes); false for an unknown name. */
static bool flag_of(const gw_captive_page_t *v, const char *t, size_t n, bool *out)
{
    if (same(t, n, "https")) {
        *out = v->https;
    } else if (same(t, n, "claim")) {
        *out = v->claim;
    } else if (same(t, n, "mdns")) {
        *out = v->mdns != NULL && v->mdns[0] != '\0';
    } else {
        return false;
    }
    return true;
}

/* @p s as HTML text: & < > " ' as entities. */
static bool out_escaped(const char *s, gw_captive_out_t out, void *ctx)
{
    size_t start = 0, i = 0;
    for (; s[i] != '\0'; i++) {
        const char *e = s[i] == '&'    ? "&amp;"
                        : s[i] == '<'  ? "&lt;"
                        : s[i] == '>'  ? "&gt;"
                        : s[i] == '"'  ? "&quot;"
                        : s[i] == '\'' ? "&#39;"
                                       : NULL;
        if (e == NULL) {
            continue;
        }
        if ((i > start && !out(ctx, s + start, i - start)) || !out(ctx, e, strlen(e))) {
            return false;
        }
        start = i + 1u;
    }
    return i == start || out(ctx, s + start, i - start);
}

bool gw_captive_render(const char *tpl, size_t len, const gw_captive_page_t *v,
                       gw_captive_out_t out, void *ctx)
{
    if (tpl == NULL || v == NULL || out == NULL) {
        return false;
    }
    /* on[d]: the text at depth d goes out; name[d]: the section that opened depth d */
    bool on[GW_CAPTIVE_DEPTH + 1];
    const char *name[GW_CAPTIVE_DEPTH + 1];
    size_t name_len[GW_CAPTIVE_DEPTH + 1];
    size_t d = 0, start = 0, i = 0;
    on[0] = true;
    name[0] = "";
    name_len[0] = 0;
    while (i + 1u < len) {
        if (tpl[i] != '{' || tpl[i + 1u] != '{') {
            i++;
            continue;
        }
        if (on[d] && i > start && !out(ctx, tpl + start, i - start)) {
            return false;
        }
        size_t t = i + 2u, e = t;
        while (e + 1u < len && (tpl[e] != '}' || tpl[e + 1u] != '}')) {
            e++;
        }
        if (e + 1u >= len) {
            return false;   /* "{{" without "}}" */
        }
        char kind = tpl[t];
        if (kind == '#' || kind == '^') {
            bool cond;
            if (d == GW_CAPTIVE_DEPTH || !flag_of(v, tpl + t + 1, e - t - 1u, &cond)) {
                return false;
            }
            d++;
            on[d] = on[d - 1u] && (kind == '#' ? cond : !cond);
            name[d] = tpl + t + 1;
            name_len[d] = e - t - 1u;
        } else if (kind == '/') {
            if (d == 0 || name_len[d] != e - t - 1u ||
                strncmp(name[d], tpl + t + 1, name_len[d]) != 0) {
                return false;
            }
            d--;
        } else {
            const char *s;
            if (!value_of(v, tpl + t, e - t, &s)) {
                return false;
            }
            if (on[d] && !out_escaped(s, out, ctx)) {
                return false;
            }
        }
        i = e + 2u;
        start = i;
    }
    if (on[d] && len > start && !out(ctx, tpl + start, len - start)) {
        return false;
    }
    return d == 0;
}
