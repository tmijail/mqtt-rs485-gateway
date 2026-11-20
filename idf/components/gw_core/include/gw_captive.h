/*
 * NET-5, issue #274: the captive portal of the commissioning access point. A phone that connects
 * to the access point opens its captive portal window by itself, and that window shows only a
 * guide page over plain HTTP: the node name, the address of the secure web UI and a button that
 * opens it. The page has no form and no secret: a password never goes over plain HTTP.
 *
 *   - DNS (gw_captive_dns_answer): while the access point runs, a small DNS server on it answers
 *     each A query of a client of the access point with the address of the access point. Other
 *     types (AAAA, HTTPS, ...) get an answer with no records (NOERROR, no data): a client then
 *     uses the A record. A packet that is not a standard query with one question gets no answer.
 *   - HTTP (gw_captive_route): the plain HTTP port answers the probe requests of the operating
 *     systems (gw_captive_probe_path), and each request for a Host that is not the gateway, with
 *     a redirect to the guide page (GW_CAPTIVE_PATH). Only for a client of the access point and
 *     only for GET; every other request keeps its behaviour (the redirect to HTTPS, or the web UI
 *     when web.https is false).
 *   - The page (gw_captive_render): the embedded template www/portal.html, with its values.
 *
 * Pure logic, no platform calls: the caller gives the packets, the addresses and the texts.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- DNS ---------- */

#define GW_CAPTIVE_DNS_PORT 53
/* The largest query that gets an answer: the classic limit of a DNS message over UDP. A query
 * with one question has less than 300 bytes, also with an EDNS option. */
#define GW_CAPTIVE_DNS_QUERY_MAX 512
/* The answer is the header and the question of the query (without its other records), plus one
 * A record of this size: a name pointer to the question, type, class, TTL, length, address. */
#define GW_CAPTIVE_DNS_A_RR 16
/* The TTL of the A record (s). Short: a phone that leaves the access point does not keep the
 * address of the gateway for the names of the internet for long. */
#define GW_CAPTIVE_DNS_TTL_S 10

/**
 * The answer to the DNS query @p q (@p len bytes, a UDP payload) in @p out (@p cap bytes; not
 * the same memory as @p q). An A query of class IN gets one A record with @p ip (network byte
 * order); any other type or class gets an answer with no records. The answer has the ID, the
 * question and the RD bit of the query, with QR and AA set, RCODE 0 (NOERROR), and no
 * authority or additional records (an EDNS option of the query is not copied).
 *
 * Returns the length of the answer (at most len + GW_CAPTIVE_DNS_A_RR), or 0 for no answer: a
 * message shorter than its header or longer than GW_CAPTIVE_DNS_QUERY_MAX, a response (QR set),
 * an opcode other than QUERY, a question count other than 1, a name with a compression pointer
 * or a label type other than 0, a label or name over its limit (63, 255), a question past the
 * end, or an answer that does not fit in @p cap.
 */
size_t gw_captive_dns_answer(const uint8_t *q, size_t len, const uint8_t ip[4], uint8_t *out,
                             size_t cap);

/* ---------- HTTP ---------- */

/* The guide page, on the plain HTTP port. */
#define GW_CAPTIVE_PATH "/portal"

typedef enum {
    GW_CAPTIVE_NONE = 0,     /* not for the portal: the request keeps its behaviour */
    GW_CAPTIVE_PAGE,         /* answer with the guide page */
    GW_CAPTIVE_REDIRECT,     /* answer with a redirect to the guide page */
} gw_captive_route_t;

/**
 * True when the path of @p uri (up to '?' or '#') is a captive portal probe of an operating
 * system or a browser: Android and Chrome (/generate_204, /gen_204), Apple (/hotspot-detect.html,
 * /library/test/success.html), Windows (/ncsi.txt, /connecttest.txt, /redirect), Firefox
 * (/canonical.html, /success.txt), GNOME (/check_network_status.txt), Kindle
 * (/kindle-wifi/wifistub.html). Case is not significant.
 */
bool gw_captive_probe_path(const char *uri);

/**
 * True when the Host header @p host names this gateway: the local address of the connection
 * (@p local_ip, dotted IPv4), the host name @p hostname, or "<hostname>.local". Case and a final
 * dot are not significant; a port (":80") is removed. An empty Host (HTTP/1.0) names the
 * gateway. NULL (the header did not fit in the buffer of the caller) does not.
 */
bool gw_captive_host_is_gateway(const char *host, const char *local_ip, const char *hostname);

/**
 * What the plain HTTP port does with a request. @p from_ap: the client is a client of the
 * running access point (the rule of the claim window, gw_claim_from_ap). @p get: the method is
 * GET. A request that is not from the access point, or not a GET, is GW_CAPTIVE_NONE. Then
 * GW_CAPTIVE_PATH is the page, and a probe path or a Host that is not the gateway
 * (gw_captive_host_is_gateway) is the redirect. Any other request is GW_CAPTIVE_NONE.
 */
gw_captive_route_t gw_captive_route(bool from_ap, bool get, const char *uri, const char *host,
                                    const char *local_ip, const char *hostname);

/* ---------- the guide page ---------- */

typedef struct {
    const char *node;     /* the node name: the edge node ID, or the host name without a
                           * configuration */
    const char *url;      /* the address of the web UI over the access point */
    const char *mdns;     /* its mDNS address; NULL or "" when mDNS is off */
    const char *ssid;     /* the network name of the access point */
    bool https;           /* the web UI uses HTTPS */
    bool claim;           /* the claim window is open: the access point has no key */
} gw_captive_page_t;

/* Writes @p n bytes of the page; false stops the page (a send error). */
typedef bool (*gw_captive_out_t)(void *ctx, const char *s, size_t n);

/* The deepest nesting of the sections of a template. */
#define GW_CAPTIVE_DEPTH 4

/**
 * Write the template @p tpl (@p len bytes) with the values of @p v through @p out.
 *   {{node}} {{url}} {{mdns}} {{ssid}}   the value, with & < > " ' as HTML entities
 *   {{#https}} ... {{/https}}            the text only with HTTPS; {{^https}}: only without it
 *   {{#claim}}, {{^claim}}               the same for the claim window
 *   {{#mdns}}, {{^mdns}}                 the same for the mDNS address
 * Sections can nest, up to GW_CAPTIVE_DEPTH. False for an unknown tag, a "{{" without "}}", a
 * section that is not closed or closed with another name, a nesting deeper than the limit, or a
 * false from @p out; the page then stops at that point.
 */
bool gw_captive_render(const char *tpl, size_t len, const gw_captive_page_t *v,
                       gw_captive_out_t out, void *ctx);

#ifdef __cplusplus
}
#endif
