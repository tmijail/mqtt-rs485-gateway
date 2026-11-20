/*
 * Issue #274 (NET-5): the captive portal of the access point. The DNS queries of a client of the
 * access point (gw_captive_dns_answer), the request line and the Host header on the plain HTTP
 * port (gw_captive_route, gw_captive_probe_path, gw_captive_host_is_gateway), and the guide page
 * with values from outside (gw_captive_render: the node name comes from the configuration).
 *
 * Input: a selector byte (bits 0-1: the function; the other bits: options), then the data.
 *   0  DNS: the address (4 bytes), then the query (the rest)
 *   1  HTTP: the URI, the Host, the local address and the host name, each ended by a NUL byte
 *   2  the page: the node name and the SSID (each ended by a NUL byte), then the template
 */
#include "fuzz.h"
#include "gw_captive.h"

#define SEL_DNS  0u
#define SEL_HTTP 1u
#define SEL_PAGE 2u

static unsigned rd16(const uint8_t *p)
{
    return ((unsigned)p[0] << 8) | p[1];
}

static void dns(fz_t *f, unsigned opt)
{
    uint8_t ip[4];
    for (size_t i = 0; i < sizeof(ip); i++) {
        ip[i] = fz_u8(f);
    }
    size_t len;
    const uint8_t *p = fz_rest(f, &len);
    uint8_t *q = fz_dup(p, len);   /* a read past the query is a finding */
    /* the room of the answer: the firmware gives len + GW_CAPTIVE_DNS_A_RR, the options less */
    size_t cap = len + GW_CAPTIVE_DNS_A_RR;
    cap = opt < 32u ? cap - (cap < opt ? cap : opt) : cap;
    uint8_t *a = malloc(cap != 0 ? cap : 1);
    if (a == NULL) {
        abort();
    }
    size_t n = gw_captive_dns_answer(q, len, ip, a, cap);
    FZ_CHECK(n <= cap);
    if (n != 0) {
        /* a query that gets an answer: a standard query with one question */
        FZ_CHECK(len >= 12 && len <= GW_CAPTIVE_DNS_QUERY_MAX);
        FZ_CHECK((q[2] & 0xF8u) == 0 && rd16(&q[4]) == 1);
        FZ_CHECK(n >= 12 + 5 && n <= len + GW_CAPTIVE_DNS_A_RR);
        /* the answer: the same ID, QR and AA, the RD of the query, NOERROR, one question */
        FZ_CHECK(a[0] == q[0] && a[1] == q[1]);
        FZ_CHECK(a[2] == (0x84u | (q[2] & 1u)) && a[3] == 0);
        FZ_CHECK(rd16(&a[4]) == 1 && rd16(&a[8]) == 0 && rd16(&a[10]) == 0);
        unsigned an = rd16(&a[6]);
        FZ_CHECK(an <= 1);
        size_t qend = n - (an != 0 ? GW_CAPTIVE_DNS_A_RR : 0u);
        FZ_CHECK(memcmp(&a[12], &q[12], qend - 12) == 0);
        if (an != 0) {
            FZ_CHECK(rd16(&a[n - 16]) == 0xC00Cu && rd16(&a[n - 14]) == 1 &&
                     rd16(&a[n - 12]) == 1 && rd16(&a[n - 6]) == 4);
            FZ_CHECK(memcmp(&a[n - 4], ip, 4) == 0);
        }
        /* an answer is never answered: no loop between two servers */
        uint8_t again[GW_CAPTIVE_DNS_QUERY_MAX + GW_CAPTIVE_DNS_A_RR];
        FZ_CHECK(gw_captive_dns_answer(a, n, ip, again, sizeof(again)) == 0);
    }
    free(a);
    free(q);
}

static void http(fz_t *f, unsigned opt)
{
    char *uri = fz_cstr(f);
    char *host = fz_cstr(f);
    char *local = fz_cstr(f);
    char *name = fz_cstr(f);
    const char *h = (opt & 1u) != 0 ? NULL : host;   /* a Host that did not fit */
    bool from_ap = (opt & 2u) != 0, get = (opt & 4u) != 0;
    bool probe = gw_captive_probe_path(uri);
    bool gw = gw_captive_host_is_gateway(h, local, name);
    gw_captive_route_t r = gw_captive_route(from_ap, get, uri, h, local, name);
    FZ_CHECK(r == GW_CAPTIVE_NONE || r == GW_CAPTIVE_PAGE || r == GW_CAPTIVE_REDIRECT);
    if (!from_ap || !get) {
        FZ_CHECK(r == GW_CAPTIVE_NONE);
    } else if (r == GW_CAPTIVE_NONE) {
        FZ_CHECK(!probe && gw);
    } else if (r == GW_CAPTIVE_REDIRECT) {
        FZ_CHECK(probe || !gw);
    } else {
        FZ_CHECK(strncmp(uri, GW_CAPTIVE_PATH, sizeof(GW_CAPTIVE_PATH) - 1) == 0);
    }
    FZ_CHECK(gw_captive_host_is_gateway(NULL, local, name) == false);
    free(name);
    free(local);
    free(host);
    free(uri);
}

typedef struct {
    size_t calls;
    size_t fail_at;   /* the write that fails (0 = none) */
    bool in_value;
} page_out_t;

static bool page_out(void *ctx, const char *s, size_t n)
{
    page_out_t *o = ctx;
    o->calls++;
    if (o->fail_at != 0 && o->calls == o->fail_at) {
        return false;
    }
    FZ_CHECK(n != 0 && s != NULL);
    if (o->in_value) {
        /* a value is HTML text: no markup character goes out as it is */
        for (size_t i = 0; i < n; i++) {
            FZ_CHECK(s[i] != '<' && s[i] != '>' && s[i] != '"' && s[i] != '\'');
            FZ_CHECK(s[i] != '&' || (n >= 4 && s[n - 1] == ';' && i == 0));
        }
    }
    return true;
}

static void page(fz_t *f, unsigned opt)
{
    char *node = fz_cstr(f);
    char *ssid = fz_cstr(f);
    size_t len;
    const uint8_t *p = fz_rest(f, &len);
    char *tpl = (char *)fz_dup(p, len);   /* no NUL: the length is an argument */
    gw_captive_page_t v = {.node = node, .url = "https://192.168.4.1/",
                           .mdns = (opt & 1u) != 0 ? "https://gw01.local/" : NULL, .ssid = ssid,
                           .https = (opt & 2u) != 0, .claim = (opt & 4u) != 0};
    page_out_t o = {.fail_at = opt >> 3};
    (void)gw_captive_render(tpl, len, &v, page_out, &o);
    /* the values alone, with the escape check of each write */
    page_out_t ov = {.in_value = true};
    static const char t_node[] = "{{node}}", t_ssid[] = "{{ssid}}";
    FZ_CHECK(gw_captive_render(t_node, sizeof(t_node) - 1, &v, page_out, &ov));
    FZ_CHECK(gw_captive_render(t_ssid, sizeof(t_ssid) - 1, &v, page_out, &ov));
    free(tpl);
    free(ssid);
    free(node);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fz_t f = {data, size};
    uint8_t sel = fz_u8(&f);
    unsigned opt = sel >> 2;
    switch (sel & 3u) {
    case SEL_DNS:
        dns(&f, opt);
        break;
    case SEL_HTTP:
        http(&f, opt);
        break;
    case SEL_PAGE:
        page(&f, opt);
        break;
    default:
        break;
    }
    return 0;
}
