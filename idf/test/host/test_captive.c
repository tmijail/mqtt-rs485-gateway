/* NET-5, issue #274: the captive portal of the access point (gw_captive.h). The DNS answer
 * (A record, no records for other types, no answer for what is not a standard query with one
 * question), the list of probe paths, the Host rule, the route of a request on the plain HTTP
 * port, and the guide page (www/portal.html) with its values. */
#include <stdio.h>
#include <string.h>

#include "gw_captive.h"
#include "support.h"
#include "unity.h"

void setUp(void) {}

void tearDown(void) {}

static const uint8_t AP_IP[4] = {192, 168, 4, 1};

/* A query for @p name with @p qtype and @p qclass, ID 0x1234, RD set; @p opt adds an EDNS OPT
 * record (ARCOUNT 1). Returns its length. */
static size_t query(uint8_t *q, const char *name, unsigned qtype, unsigned qclass, bool opt)
{
    static const uint8_t hdr[12] = {0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0};
    memcpy(q, hdr, sizeof(hdr));
    size_t n = sizeof(hdr);
    const char *s = name;
    while (*s != '\0') {
        const char *dot = strchr(s, '.');
        size_t l = dot != NULL ? (size_t)(dot - s) : strlen(s);
        q[n++] = (uint8_t)l;
        memcpy(&q[n], s, l);
        n += l;
        s += l + (dot != NULL ? 1u : 0u);
    }
    q[n++] = 0;
    q[n++] = (uint8_t)(qtype >> 8);
    q[n++] = (uint8_t)qtype;
    q[n++] = (uint8_t)(qclass >> 8);
    q[n++] = (uint8_t)qclass;
    if (opt) {
        static const uint8_t rr[11] = {0, 0, 41, 0x04, 0xD0, 0, 0, 0, 0, 0, 0};
        memcpy(&q[n], rr, sizeof(rr));
        n += sizeof(rr);
        q[11] = 1;
    }
    return n;
}

static unsigned u16(const uint8_t *p)
{
    return ((unsigned)p[0] << 8) | p[1];
}

/* An A query gets one A record with the address of the access point, after its question. */
static void test_dns_a_record(void)
{
    uint8_t q[600], a[700];
    size_t n = query(q, "connectivitycheck.gstatic.com", 1, 1, false);
    size_t m = gw_captive_dns_answer(q, n, AP_IP, a, sizeof(a));
    TEST_ASSERT_EQUAL_size_t(n + GW_CAPTIVE_DNS_A_RR, m);
    TEST_ASSERT_EQUAL_HEX8(0x12, a[0]);
    TEST_ASSERT_EQUAL_HEX8(0x34, a[1]);
    TEST_ASSERT_EQUAL_HEX8(0x85, a[2]);   /* QR, opcode 0, AA, RD of the query */
    TEST_ASSERT_EQUAL_HEX8(0x00, a[3]);   /* RCODE NOERROR */
    TEST_ASSERT_EQUAL_UINT(1, u16(&a[4]));
    TEST_ASSERT_EQUAL_UINT(1, u16(&a[6]));
    TEST_ASSERT_EQUAL_UINT(0, u16(&a[8]));
    TEST_ASSERT_EQUAL_UINT(0, u16(&a[10]));
    TEST_ASSERT_EQUAL_MEMORY(&q[12], &a[12], n - 12);   /* the question */
    const uint8_t rr[16] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, GW_CAPTIVE_DNS_TTL_S, 0, 4,
                            192, 168, 4, 1};
    TEST_ASSERT_EQUAL_MEMORY(rr, &a[n], sizeof(rr));
    /* RD 0 in the query: RD 0 in the answer */
    q[2] = 0;
    TEST_ASSERT_EQUAL_size_t(m, gw_captive_dns_answer(q, n, AP_IP, a, sizeof(a)));
    TEST_ASSERT_EQUAL_HEX8(0x84, a[2]);
    /* the address comes from the caller (the interface of the query) */
    const uint8_t eth[4] = {10, 0, 2, 15};
    TEST_ASSERT_EQUAL_size_t(m, gw_captive_dns_answer(q, n, eth, a, sizeof(a)));
    TEST_ASSERT_EQUAL_MEMORY(eth, &a[m - 4], 4);
}

/* An EDNS OPT record of the query is not in the answer (ARCOUNT 0). */
static void test_dns_edns_option_not_copied(void)
{
    uint8_t q[600], a[700];
    size_t n = query(q, "captive.apple.com", 1, 1, true);
    size_t m = gw_captive_dns_answer(q, n, AP_IP, a, sizeof(a));
    TEST_ASSERT_EQUAL_size_t(n - 11 + GW_CAPTIVE_DNS_A_RR, m);
    TEST_ASSERT_EQUAL_UINT(1, u16(&a[6]));
    TEST_ASSERT_EQUAL_UINT(0, u16(&a[10]));
}

/* AAAA, HTTPS, PTR, ANY and an A query of another class: NOERROR with no records. */
static void test_dns_other_types_no_records(void)
{
    static const unsigned types[][2] = {{28, 1}, {65, 1}, {12, 1}, {255, 1}, {1, 3}, {1, 255}};
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        uint8_t q[600], a[700];
        size_t n = query(q, "www.msftconnecttest.com", types[i][0], types[i][1], true);
        size_t m = gw_captive_dns_answer(q, n, AP_IP, a, sizeof(a));
        TEST_ASSERT_EQUAL_size_t(n - 11, m);
        TEST_ASSERT_EQUAL_HEX8(0x85, a[2]);
        TEST_ASSERT_EQUAL_HEX8(0x00, a[3]);
        TEST_ASSERT_EQUAL_UINT(1, u16(&a[4]));
        TEST_ASSERT_EQUAL_UINT(0, u16(&a[6]));
        TEST_ASSERT_EQUAL_UINT(0, u16(&a[10]));
    }
}

/* What is not a standard query with one valid question gets no answer. */
static void test_dns_no_answer(void)
{
    uint8_t q[600], a[700];
    size_t n = query(q, "example.com", 1, 1, false);
    TEST_ASSERT_NOT_EQUAL(0, gw_captive_dns_answer(q, n, AP_IP, a, sizeof(a)));
    /* shorter than the header, and every cut of the question */
    for (size_t k = 0; k < n; k++) {
        TEST_ASSERT_EQUAL_size_t(0, gw_captive_dns_answer(q, k, AP_IP, a, sizeof(a)));
    }
    /* a response (QR), another opcode (IQUERY, STATUS, NOTIFY, UPDATE) */
    q[2] = 0x81;
    TEST_ASSERT_EQUAL_size_t(0, gw_captive_dns_answer(q, n, AP_IP, a, sizeof(a)));
    for (unsigned op = 1; op < 16; op++) {
        q[2] = (uint8_t)(op << 3);
        TEST_ASSERT_EQUAL_size_t(0, gw_captive_dns_answer(q, n, AP_IP, a, sizeof(a)));
    }
    q[2] = 0x01;
    /* QDCOUNT 0 or 2 */
    q[5] = 0;
    TEST_ASSERT_EQUAL_size_t(0, gw_captive_dns_answer(q, n, AP_IP, a, sizeof(a)));
    q[5] = 2;
    TEST_ASSERT_EQUAL_size_t(0, gw_captive_dns_answer(q, n, AP_IP, a, sizeof(a)));
    q[4] = 1;
    q[5] = 1;
    TEST_ASSERT_EQUAL_size_t(0, gw_captive_dns_answer(q, n, AP_IP, a, sizeof(a)));
    q[4] = 0;
    /* a compression pointer and the reserved label types in the question */
    for (unsigned t = 0x40; t <= 0xC0; t += 0x40) {
        q[12] = (uint8_t)t;
        TEST_ASSERT_EQUAL_size_t(0, gw_captive_dns_answer(q, n, AP_IP, a, sizeof(a)));
    }
    q[12] = 7;
    TEST_ASSERT_NOT_EQUAL(0, gw_captive_dns_answer(q, n, AP_IP, a, sizeof(a)));
    /* the answer does not fit */
    TEST_ASSERT_EQUAL_size_t(0, gw_captive_dns_answer(q, n, AP_IP, a, n + 15));
    TEST_ASSERT_NOT_EQUAL(0, gw_captive_dns_answer(q, n, AP_IP, a, n + 16));
    /* longer than GW_CAPTIVE_DNS_QUERY_MAX (padding after the question) */
    memset(&q[n], 0, sizeof(q) - n);
    TEST_ASSERT_NOT_EQUAL(0, gw_captive_dns_answer(q, GW_CAPTIVE_DNS_QUERY_MAX, AP_IP, a,
                                                   sizeof(a)));
    TEST_ASSERT_EQUAL_size_t(0, gw_captive_dns_answer(q, GW_CAPTIVE_DNS_QUERY_MAX + 1, AP_IP, a,
                                                      sizeof(a)));
    /* NULL */
    TEST_ASSERT_EQUAL_size_t(0, gw_captive_dns_answer(NULL, n, AP_IP, a, sizeof(a)));
    TEST_ASSERT_EQUAL_size_t(0, gw_captive_dns_answer(q, n, NULL, a, sizeof(a)));
    TEST_ASSERT_EQUAL_size_t(0, gw_captive_dns_answer(q, n, AP_IP, NULL, sizeof(a)));
}

/* A label has at most 63 bytes, a name at most 255 (with the root label). */
static void test_dns_name_limits(void)
{
    uint8_t q[600], a[700];
    char name[300];
    memset(name, 'a', 63);
    name[63] = '\0';
    size_t n = query(q, name, 1, 1, false);
    TEST_ASSERT_NOT_EQUAL(0, gw_captive_dns_answer(q, n, AP_IP, a, sizeof(a)));
    memset(name, 'a', 64);
    name[64] = '\0';
    n = query(q, name, 1, 1, false);
    TEST_ASSERT_EQUAL_size_t(0, gw_captive_dns_answer(q, n, AP_IP, a, sizeof(a)));
    /* 4 labels of 62 bytes and one of 2: 4 * 63 + 3 + 1 = 256 bytes; one of 1: 255 */
    char *p = name;
    for (int k = 0; k < 4; k++) {
        memset(p, 'b', 62);
        p[62] = '.';
        p += 63;
    }
    strcpy(p, "cc");
    n = query(q, name, 1, 1, false);
    TEST_ASSERT_EQUAL_size_t(0, gw_captive_dns_answer(q, n, AP_IP, a, sizeof(a)));
    strcpy(p, "c");
    n = query(q, name, 1, 1, false);
    TEST_ASSERT_EQUAL_size_t(12 + 255 + 4, n);
    TEST_ASSERT_EQUAL_size_t(n + 16, gw_captive_dns_answer(q, n, AP_IP, a, sizeof(a)));
    /* the root name (a query for ".") */
    n = query(q, "", 2, 1, false);
    TEST_ASSERT_EQUAL_size_t(n, gw_captive_dns_answer(q, n, AP_IP, a, sizeof(a)));
}

/* The probe paths of Android, iOS, macOS, Windows and the browsers; a query string does not
 * change the path. */
static void test_probe_paths(void)
{
    static const char *const yes[] = {
        "/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html",
        "/ncsi.txt", "/connecttest.txt", "/redirect", "/canonical.html", "/success.txt",
        "/check_network_status.txt", "/kindle-wifi/wifistub.html", "/generate_204?x=1",
        "/Hotspot-Detect.html", "/success.txt?ipv4", "/gen_204#a"};
    static const char *const no[] = {
        "/", "", "/portal", "/claim", "/api/status", "/generate_2040", "/generate_20",
        "/generate_204/", "generate_204", "/hotspot-detect.htm", "/x/generate_204",
        "/favicon.ico", "/index.html"};
    for (size_t i = 0; i < sizeof(yes) / sizeof(yes[0]); i++) {
        TEST_ASSERT_TRUE_MESSAGE(gw_captive_probe_path(yes[i]), yes[i]);
    }
    for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(gw_captive_probe_path(no[i]), no[i]);
    }
    TEST_ASSERT_FALSE(gw_captive_probe_path(NULL));
}

/* The Host names the gateway: its address on this connection, its host name, its mDNS name. */
static void test_host_is_gateway(void)
{
    static const char *const yes[] = {"192.168.4.1", "192.168.4.1:80", "gw01", "GW01",
                                      "gw01.local", "GW01.Local", "gw01.local.", "gw01.local:8080",
                                      "gw01.", ""};
    static const char *const no[] = {
        "connectivitycheck.gstatic.com", "captive.apple.com", "www.msftconnecttest.com",
        "192.168.4.10", "192.168.4.1x", "10.0.2.15", "gw01x", "gw01.local.x", "x.gw01.local",
        "gw01.locall", ".local", "local", ":80", ".", "[fe80::1]", "[fe80::1]:80", "gw0"};
    for (size_t i = 0; i < sizeof(yes) / sizeof(yes[0]); i++) {
        TEST_ASSERT_TRUE_MESSAGE(gw_captive_host_is_gateway(yes[i], "192.168.4.1", "gw01"),
                                 yes[i]);
    }
    for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(gw_captive_host_is_gateway(no[i], "192.168.4.1", "gw01"),
                                  no[i]);
    }
    /* a Host that did not fit in the buffer of the caller */
    TEST_ASSERT_FALSE(gw_captive_host_is_gateway(NULL, "192.168.4.1", "gw01"));
    /* no host name, no local address: only the other one counts */
    TEST_ASSERT_TRUE(gw_captive_host_is_gateway("192.168.4.1", "192.168.4.1", ""));
    TEST_ASSERT_FALSE(gw_captive_host_is_gateway(".local", "192.168.4.1", ""));
    TEST_ASSERT_TRUE(gw_captive_host_is_gateway("gw01.local", NULL, "gw01"));
    TEST_ASSERT_FALSE(gw_captive_host_is_gateway("192.168.4.1", NULL, NULL));
}

/* Only a GET of a client of the access point goes to the portal. */
static void test_route(void)
{
    const char *ip = "192.168.4.1", *h = "gw01";
    /* the page, also with a Host that is not the gateway (the redirect gives the address) */
    TEST_ASSERT_EQUAL(GW_CAPTIVE_PAGE, gw_captive_route(true, true, "/portal", "192.168.4.1", ip, h));
    TEST_ASSERT_EQUAL(GW_CAPTIVE_PAGE, gw_captive_route(true, true, "/portal?x", "a.example", ip, h));
    TEST_ASSERT_EQUAL(GW_CAPTIVE_PAGE, gw_captive_route(true, true, "/portal", NULL, ip, h));
    /* the probes, and each request for another Host */
    TEST_ASSERT_EQUAL(GW_CAPTIVE_REDIRECT,
                      gw_captive_route(true, true, "/generate_204", "connectivitycheck.gstatic.com",
                                       ip, h));
    TEST_ASSERT_EQUAL(GW_CAPTIVE_REDIRECT,
                      gw_captive_route(true, true, "/generate_204", "192.168.4.1", ip, h));
    TEST_ASSERT_EQUAL(GW_CAPTIVE_REDIRECT,
                      gw_captive_route(true, true, "/", "connectivity-check.ubuntu.com", ip, h));
    TEST_ASSERT_EQUAL(GW_CAPTIVE_REDIRECT,
                      gw_captive_route(true, true, "/api/status", "example.com", ip, h));
    TEST_ASSERT_EQUAL(GW_CAPTIVE_REDIRECT, gw_captive_route(true, true, "/", NULL, ip, h));
    /* the gateway itself: the usual behaviour */
    TEST_ASSERT_EQUAL(GW_CAPTIVE_NONE, gw_captive_route(true, true, "/", "192.168.4.1", ip, h));
    TEST_ASSERT_EQUAL(GW_CAPTIVE_NONE, gw_captive_route(true, true, "/claim", "gw01.local", ip, h));
    TEST_ASSERT_EQUAL(GW_CAPTIVE_NONE, gw_captive_route(true, true, "/", "", ip, h));
    TEST_ASSERT_EQUAL(GW_CAPTIVE_NONE, gw_captive_route(true, true, "/portals", "gw01", ip, h));
    /* not a client of the access point, or not a GET: never */
    const char *uris[] = {"/portal", "/generate_204", "/", "/hotspot-detect.html"};
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        TEST_ASSERT_EQUAL(GW_CAPTIVE_NONE,
                          gw_captive_route(false, true, uris[i], "captive.apple.com", ip, h));
        TEST_ASSERT_EQUAL(GW_CAPTIVE_NONE,
                          gw_captive_route(true, false, uris[i], "captive.apple.com", ip, h));
    }
    TEST_ASSERT_EQUAL(GW_CAPTIVE_NONE, gw_captive_route(true, true, NULL, "x", ip, h));
}

/* ---------- the page ---------- */

static char s_page[16384];
static size_t s_len;
static size_t s_calls_left;   /* the writer fails after this many calls */

static bool out(void *ctx, const char *s, size_t n)
{
    (void)ctx;
    if (s_calls_left == 0) {
        return false;
    }
    s_calls_left--;
    TEST_ASSERT_TRUE(s_len + n < sizeof(s_page));
    memcpy(&s_page[s_len], s, n);
    s_len += n;
    s_page[s_len] = '\0';
    return true;
}

static bool render(const char *tpl, size_t len, const gw_captive_page_t *v)
{
    s_len = 0;
    s_page[0] = '\0';
    s_calls_left = SIZE_MAX;
    return gw_captive_render(tpl, len, v, out, NULL);
}

static bool render_s(const char *tpl, const gw_captive_page_t *v)
{
    return render(tpl, strlen(tpl), v);
}

/* The tags and the sections. */
static void test_render_template(void)
{
    gw_captive_page_t v = {.node = "GW01", .url = "https://192.168.4.1/", .mdns = "",
                           .ssid = "GW-Setup-A1B2C3", .https = true, .claim = false};
    TEST_ASSERT_TRUE(render_s("a{{node}}b{{url}}c{{ssid}}d{{mdns}}e", &v));
    TEST_ASSERT_EQUAL_STRING("aGW01bhttps://192.168.4.1/cGW-Setup-A1B2C3de", s_page);
    TEST_ASSERT_TRUE(render_s("{{#https}}S{{/https}}{{^https}}P{{/https}}"
                              "{{#claim}}C{{/claim}}{{^claim}}c{{/claim}}"
                              "{{#mdns}}M{{/mdns}}{{^mdns}}m{{/mdns}}", &v));
    TEST_ASSERT_EQUAL_STRING("Scm", s_page);
    v.https = false;
    v.claim = true;
    v.mdns = "http://gw01.local/";
    TEST_ASSERT_TRUE(render_s("{{#https}}S{{/https}}{{^https}}P{{/https}}"
                              "{{#claim}}C{{/claim}}{{^claim}}c{{/claim}}"
                              "{{#mdns}}M{{mdns}}{{/mdns}}{{^mdns}}m{{/mdns}}", &v));
    TEST_ASSERT_EQUAL_STRING("PCMhttp://gw01.local/", s_page);
    /* nesting: an inner true section in an outer false one stays out */
    v.claim = false;
    TEST_ASSERT_TRUE(render_s("<{{#claim}}a{{#mdns}}b{{node}}{{/mdns}}c{{/claim}}>"
                              "{{#mdns}}{{^claim}}d{{/claim}}{{/mdns}}", &v));
    TEST_ASSERT_EQUAL_STRING("<>d", s_page);
    /* NULL values are empty; text without tags; single braces */
    v.node = NULL;
    v.mdns = NULL;
    TEST_ASSERT_TRUE(render_s("[{{node}}]{{#mdns}}x{{/mdns}}{ } }}{", &v));
    TEST_ASSERT_EQUAL_STRING("[]{ } }}{", s_page);
    TEST_ASSERT_TRUE(render_s("", &v));
    TEST_ASSERT_EQUAL_STRING("", s_page);
}

/* The values are HTML text: a node name cannot add markup to the page. */
static void test_render_escapes(void)
{
    gw_captive_page_t v = {.node = "<script>alert('x')</script>&\"", .url = "\"><b>",
                           .ssid = "a&b", .https = true};
    TEST_ASSERT_TRUE(render_s("{{node}}|{{url}}|{{ssid}}", &v));
    TEST_ASSERT_EQUAL_STRING("&lt;script&gt;alert(&#39;x&#39;)&lt;/script&gt;&amp;&quot;|"
                             "&quot;&gt;&lt;b&gt;|a&amp;b", s_page);
}

/* A template error stops the page: unknown tags, sections that do not match, too deep. */
static void test_render_errors(void)
{
    gw_captive_page_t v = {.node = "GW01", .https = true};
    /* five sections: one more than GW_CAPTIVE_DEPTH */
    static const char too_deep[] = "{{#https}}{{#https}}{{#https}}{{#https}}{{#https}}x"
                                   "{{/https}}{{/https}}{{/https}}{{/https}}{{/https}}";
    static const char *const bad[] = {
        "{{nodes}}", "{{}}", "{{#}}x{{/}}", "{{#node}}x{{/node}}", "{{#https}}x",
        "x{{/https}}", "{{#https}}x{{/claim}}", "{{#https}}x{{/http}}", "{{node", "a{{",
        too_deep, "{{Node}}x{{nodex}}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(render_s(bad[i], &v), bad[i]);
    }
    /* the deepest nesting that is accepted */
    TEST_ASSERT_TRUE(render_s("{{#https}}{{#https}}{{#https}}{{#https}}x{{/https}}{{/https}}"
                              "{{/https}}{{/https}}", &v));
    TEST_ASSERT_EQUAL_STRING("x", s_page);
    TEST_ASSERT_FALSE(gw_captive_render(NULL, 0, &v, out, NULL));
    TEST_ASSERT_FALSE(gw_captive_render("x", 1, NULL, out, NULL));
    TEST_ASSERT_FALSE(gw_captive_render("x", 1, &v, NULL, NULL));
    /* a send error stops the page at once */
    static const char tpl[] = "a{{node}}b{{#https}}c{{/https}}d<";
    for (size_t k = 0; k < 4; k++) {
        s_len = 0;
        s_calls_left = k;
        TEST_ASSERT_FALSE(gw_captive_render(tpl, strlen(tpl), &v, out, NULL));
        TEST_ASSERT_TRUE(s_len <= 6);
    }
    v.node = "<&>";   /* three entities: three writes */
    for (size_t k = 0; k < 3; k++) {
        s_len = 0;
        s_calls_left = k;
        TEST_ASSERT_FALSE(gw_captive_render("{{node}}", 8, &v, out, NULL));
    }
}

static size_t count(const char *hay, const char *needle)
{
    size_t n = 0;
    for (const char *p = strstr(hay, needle); p != NULL; p = strstr(p + 1, needle)) {
        n++;
    }
    return n;
}

/* The page of the firmware (www/portal.html), as the approved prototype of #274 gives it: the
 * node name, the address, the button and the certificate note; the note of the claim window
 * only during the window; the mDNS address only with mDNS; no form, no password field. */
static void test_portal_page(void)
{
    size_t len;
    const char *tpl = support_read_file(GW_PORTAL_HTML, &len);
    gw_captive_page_t v = {.node = "GW01", .url = "https://192.168.4.1/", .mdns = NULL,
                           .ssid = "GW-Setup-A1B2C3", .https = true, .claim = false};
    TEST_ASSERT_TRUE(render(tpl, len, &v));
    TEST_ASSERT_NULL(strstr(s_page, "{{"));   /* each tag is filled in ("}}" is in the CSS) */
    TEST_ASSERT_NOT_NULL(strstr(s_page, "<h1 id=\"node\">GW01</h1>"));
    TEST_ASSERT_NOT_NULL(strstr(s_page, "To configure this gateway, open the secure web page."));
    TEST_ASSERT_NOT_NULL(strstr(s_page, "Address of the secure web page<input id=\"url\" "
                                        "type=\"text\" readonly value=\"https://192.168.4.1/\""));
    TEST_ASSERT_NOT_NULL(strstr(s_page, ">Open in the browser</button>"));
    TEST_ASSERT_NOT_NULL(strstr(s_page, "Your browser shows a certificate warning the first "
                                        "time. The gateway uses its own certificate."));
    TEST_ASSERT_NULL(strstr(s_page, "id=\"claim\""));
    TEST_ASSERT_NULL(strstr(s_page, "id=\"mdns\""));
    TEST_ASSERT_NULL(strstr(s_page, "<form"));
    TEST_ASSERT_NULL(strstr(s_page, "type=\"password\""));
    TEST_ASSERT_NULL(strstr(s_page, "fetch("));   /* no request to the API */
    /* the claim window, and mDNS */
    v.claim = true;
    v.mdns = "https://gw01.local/";
    TEST_ASSERT_TRUE(render(tpl, len, &v));
    TEST_ASSERT_NOT_NULL(strstr(s_page, "<p>The Wi-Fi network GW-Setup-A1B2C3 is open until you "
                                        "set the web password.</p>"));
    TEST_ASSERT_NOT_NULL(strstr(s_page, "<p>Set the web password only on the secure page. This "
                                        "page does not ask for a password.</p>"));
    TEST_ASSERT_NOT_NULL(strstr(s_page, "The secure web page also has the address <span "
                                        "class=\"mono\">https://gw01.local/</span>."));
    TEST_ASSERT_EQUAL_size_t(1, count(s_page, "id=\"claim\""));
    /* web.https false: no "secure", no certificate note */
    v.https = false;
    v.url = "http://192.168.4.1:8080/";
    v.mdns = "http://gw01.local:8080/";
    TEST_ASSERT_TRUE(render(tpl, len, &v));
    TEST_ASSERT_NULL(strstr(s_page, "secure web page"));
    TEST_ASSERT_NULL(strstr(s_page, "secure page"));
    TEST_ASSERT_NULL(strstr(s_page, "certificate warning"));
    TEST_ASSERT_NOT_NULL(strstr(s_page, "To configure this gateway, open the web page."));
    TEST_ASSERT_NOT_NULL(strstr(s_page, "value=\"http://192.168.4.1:8080/\""));
    TEST_ASSERT_NOT_NULL(strstr(s_page, "Set the web password only on the web page."));
    TEST_ASSERT_NOT_NULL(strstr(s_page, "The web page also has the address <span "
                                        "class=\"mono\">http://gw01.local:8080/</span>."));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_dns_a_record);
    RUN_TEST(test_dns_edns_option_not_copied);
    RUN_TEST(test_dns_other_types_no_records);
    RUN_TEST(test_dns_no_answer);
    RUN_TEST(test_dns_name_limits);
    RUN_TEST(test_probe_paths);
    RUN_TEST(test_host_is_gateway);
    RUN_TEST(test_route);
    RUN_TEST(test_render_template);
    RUN_TEST(test_render_escapes);
    RUN_TEST(test_render_errors);
    RUN_TEST(test_portal_page);
    return UNITY_END();
}
