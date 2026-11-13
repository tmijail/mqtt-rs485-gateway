#include "gw_web.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gw_auth.h"
#include "gw_authlimit.h"
#include "gw_bus.h"
#include "gw_fw.h"
#include "gw_buzz.h"
#include "gw_buzzer.h"
#include "gw_cdump.h"
#include "gw_claim.h"
#include "gw_conv.h"
#include "gw_coredump.h"
#include "gw_edge.h"
#include "gw_io.h"
#include "gw_rtc.h"
#include "gw_sfcard.h"
#include "gw_log.h"
#include "gw_mbdiag.h"
#include "gw_net.h"
#include "gw_ota.h"
#include "gw_status.h"
#include "gw_storage.h"
#include "gw_tagdb.h"
#include "gw_webcert.h"
#include "lwip/sockets.h"

static const char *TAG = "web";

#define REALM_HDR "Basic realm=\"gateway\", charset=\"UTF-8\""
#define WEB_URI_HANDLERS 20   /* the routes of gw_web_start (uris[]) */

static gw_web_ctx_t s_ctx;
static httpd_handle_t s_server;
static httpd_handle_t s_redirect;    /* issue #64: plain HTTP that only redirects to HTTPS */
static bool s_https;
static gw_authlimit_t s_limit;       /* issue #64: login attempts per source address */

/* Credentials checked by the server task only (single task: no lock needed). */
static char s_user[GW_USER_SIZE];           /* "" = any user name (recovery) */
static char s_hash[GW_PWHASH_SIZE];
static uint8_t s_ok_digest[GW_SHA256_LEN];   /* SHA-256 of the last accepted Authorization */
static bool s_ok_cached;
static uint32_t s_csrf_refused;              /* issue #100: writes refused as cross-site */

/* ---------- JSON helpers ---------- */

/* Append @p s to @p out as the body of a JSON string. */
static size_t json_escape(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    for (const unsigned char *c = (const unsigned char *)s; *c && n + 7 < cap; c++) {
        if (*c == '"' || *c == '\\') {
            out[n++] = '\\';
            out[n++] = (char)*c;
        } else if (*c < 0x20) {
            n += (size_t)snprintf(out + n, cap - n, "\\u%04x", *c);
        } else {
            out[n++] = (char)*c;
        }
    }
    out[n] = '\0';
    return n;
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const gw_cfg_err_t *err,
                            const char *msg)
{
    char m[2 * GW_CFG_ERR_SIZE], p[2 * GW_CFG_PATH_SIZE], body[3 * GW_CFG_ERR_SIZE + 160];
    json_escape(m, sizeof(m), err != NULL ? err->msg : msg);
    json_escape(p, sizeof(p), err != NULL ? err->path : "");
    snprintf(body, sizeof(body),
             "{\"ok\":false,\"error\":\"%s\",\"path\":\"%s\",\"line\":%u,\"column\":%u}\n", m, p,
             err != NULL ? err->line : 0u, err != NULL ? err->column : 0u);
    return send_json(req, status, body);
}

/* ---------- authentication ---------- */

static bool check_credentials(const char *header)
{
    char user[GW_USER_SIZE], pass[GW_CRED_SIZE];
    if (!gw_auth_parse_basic(header, user, sizeof(user), pass, sizeof(pass))) {
        return false;
    }
    bool ok;
    if (s_hash[0] != '\0') {
        ok = gw_auth_check_password(pass, s_hash) && (s_user[0] == '\0' || strcmp(user, s_user) == 0);
    } else {
        /* no web password yet: the commissioning password, for "admin" */
        const char *cp = s_ctx.commissioning_password;
        size_t n = strlen(cp);
        ok = strcmp(user, "admin") == 0 && strlen(pass) == n && gw_auth_equal(pass, cp, n);
    }
    memset(pass, 0, sizeof(pass));
    return ok;
}

/* Source address of the request: IPv4 in the first 4 bytes (also when mapped into IPv6). */
static void peer_addr(httpd_req_t *req, uint8_t out[16], char *text, size_t cap)
{
    memset(out, 0, 16);
    snprintf(text, cap, "?");
    struct sockaddr_storage a;
    socklen_t n = sizeof(a);
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&a, &n) != 0) {
        return;
    }
    if (a.ss_family == AF_INET) {
        memcpy(out, &((struct sockaddr_in *)&a)->sin_addr, 4);
        inet_ntop(AF_INET, out, text, cap);
        return;
    }
    const uint8_t *v6 = (const uint8_t *)&((struct sockaddr_in6 *)&a)->sin6_addr;
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (memcmp(v6, mapped, sizeof(mapped)) == 0) {
        memcpy(out, v6 + 12, 4);
        inet_ntop(AF_INET, out, text, cap);
    } else {
        memcpy(out, v6, 16);
        inet_ntop(AF_INET6, out, text, cap);
    }
}

/* ---------- stall watchdog (issue #110) ---------- */

/*
 * esp_http_server serves every session from one task, and it reads a request's TLS handshake
 * and headers with blocking receives (SO_RCVTIMEO = recv_wait_timeout, 5 s, per receive, not
 * per request). A client that sends them byte by byte (slowloris), or stops half-way, holds
 * the task, and with it the whole web UI and API, for as long as it keeps trickling.
 *
 * The watchdog is its own low-priority task ("web_guard", static stack), not an esp_timer
 * callback: the esp_timer task also runs the 1 kHz DI sampler (IO-4 pulse counts, REL-1), and
 * httpd_queue_work (a sendto through the tcpip thread), shutdown() and logging can block it
 * for milliseconds. Every second it queues a "ping" to each server's control socket; the
 * server task runs it when it is back in its loop and marks it alive. Our handlers mark it
 * alive on entry and exit, and a refused authenticated body that we drain marks it alive for
 * every piece that arrives. If a server has not been seen alive for CONFIG_GW_WEB_STALL_S
 * outside a handler (gw_webstall_stuck), the watchdog shuts its sessions down: the blocked
 * receive returns and httpd closes the session. Idle keep-alive sessions of the same server go
 * too: browsers reconnect. This shortens a slowloris hold to about CONFIG_GW_WEB_STALL_S; it
 * does not prevent one (README 6.2.1). The ping also records each server's task, so the shell
 * can tell the two "httpd" tasks apart.
 */
typedef struct {
    httpd_handle_t server;
    const char *name;
    uint16_t port;                     /* its listening port: a session socket's local port */
    TaskHandle_t task;                 /* the server's task, from its first ping */
    atomic_uint alive_ms;              /* last time the task was seen making progress */
    atomic_bool in_handler;            /* in one of our handlers (not draining) */
    atomic_bool ping_pending;
    atomic_uint stalls;                /* times its sessions were shut down */
} web_guard_t;

static web_guard_t s_guard[2];         /* [0] the web UI and API, [1] the HTTP redirect */
static StaticTask_t s_guard_tcb;
static StackType_t s_guard_stack[2560 / sizeof(StackType_t)];

static uint32_t now_ms32(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static web_guard_t *guard_of(httpd_handle_t server)
{
    for (size_t i = 0; i < sizeof(s_guard) / sizeof(s_guard[0]); i++) {
        if (s_guard[i].server == server) {
            return &s_guard[i];
        }
    }
    return NULL;
}

/* Every handler goes through here: the watchdog leaves a long handler alone, and a handler
 * that just returned after a long time is not mistaken for a stall before the next ping. */
typedef struct {
    esp_err_t (*fn)(httpd_req_t *req);
} handler_t;

static esp_err_t guarded(httpd_req_t *req)
{
    web_guard_t *g = guard_of(req->handle);
    if (g != NULL) {
        atomic_store(&g->alive_ms, now_ms32());
        atomic_store(&g->in_handler, true);
    }
    esp_err_t r = ((const handler_t *)req->user_ctx)->fn(req);
    if (g != NULL) {
        atomic_store(&g->alive_ms, now_ms32());
        atomic_store(&g->in_handler, false);
    }
    return r;
}

static void guard_ping(void *arg)   /* runs in the server's task, from its loop */
{
    web_guard_t *g = arg;
    g->task = xTaskGetCurrentTaskHandle();
    atomic_store(&g->alive_ms, now_ms32());
    atomic_store(&g->ping_pending, false);
}

/* Shut down the sessions of a held server. Its task is blocked on one session, so its table
 * does not change meanwhile; still, httpd_get_client_list reads it from this task, and a
 * socket number could have been reused by another component (MQTT, Modbus TCP) in between:
 * only sockets whose local port is this server's are touched. */
static unsigned guard_shut(web_guard_t *g)
{
    int fds[4];
    size_t n = sizeof(fds) / sizeof(fds[0]);
    unsigned shut = 0;
    if (httpd_get_client_list(g->server, &n, fds) != ESP_OK) {
        return 0;
    }
    for (size_t k = 0; k < n; k++) {
        struct sockaddr_storage a;
        socklen_t len = sizeof(a);
        if (getsockname(fds[k], (struct sockaddr *)&a, &len) != 0) {
            continue;
        }
        uint16_t port = a.ss_family == AF_INET ? ntohs(((struct sockaddr_in *)&a)->sin_port)
                                               : ntohs(((struct sockaddr_in6 *)&a)->sin6_port);
        if (port == g->port && shutdown(fds[k], SHUT_RDWR) == 0) {
            shut++;
        }
    }
    return shut;
}

static void guard_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        for (size_t i = 0; i < sizeof(s_guard) / sizeof(s_guard[0]); i++) {
            web_guard_t *g = &s_guard[i];
            if (g->server == NULL) {
                continue;
            }
            if (!atomic_exchange(&g->ping_pending, true) &&
                httpd_queue_work(g->server, guard_ping, g) != ESP_OK) {
                atomic_store(&g->ping_pending, false);
            }
            uint32_t now = now_ms32();
            if (!gw_webstall_stuck(now, atomic_load(&g->alive_ms), atomic_load(&g->in_handler),
                                   CONFIG_GW_WEB_STALL_S * 1000u)) {
                continue;
            }
            unsigned shut = guard_shut(g);
            atomic_fetch_add(&g->stalls, 1u);
            atomic_store(&g->alive_ms, now);   /* next look in another CONFIG_GW_WEB_STALL_S */
            ESP_LOGW(TAG, "%s server held for more than %d s by a request that does not arrive "
                          "(slow or stalled client): %u session(s) shut down",
                     g->name, CONFIG_GW_WEB_STALL_S, shut);
        }
    }
}

static void guard_add(int i, httpd_handle_t server, const char *name, uint16_t port)
{
    web_guard_t *g = &s_guard[i];
    g->name = name;
    g->port = port;
    atomic_store(&g->alive_ms, now_ms32());
    g->server = server;
}

static void guard_start(void)
{
    static bool started;
    if (CONFIG_GW_WEB_STALL_S == 0 || started) {
        return;
    }
    started = true;
    /* below the web servers (3): it only has to run when they are stuck */
    xTaskCreateStatic(guard_task, "web_guard", sizeof(s_guard_stack), NULL, 1, s_guard_stack,
                      &s_guard_tcb);
}

/* ---------- refused requests with a body (issue #106) ---------- */

/* Refused requests whose unread body was cut. Incremented by the web server task and by the
 * redirect server's task, read by the shell's: atomic. */
static atomic_uint s_cut_bodies;

static int recv_refused(httpd_handle_t hd, int sockfd, char *buf, size_t len, int flags)
{
    (void)hd, (void)sockfd, (void)buf, (void)len, (void)flags;
    return HTTPD_SOCK_ERR_FAIL;
}

/* Bodies up to CUT_MIN_BODY are drained as before: they are in the socket buffers already (a
 * few reads), the client reads the reply cleanly, and a 401 to a GET keeps its connection. */
#define CUT_MIN_BODY 1024

/*
 * An untrusted refusal (401, 429, 403: no credentials checked yet, or another site; the HTTP
 * port's redirect): the rest of the body is not read. Call before sending the reply, and only
 * before the handler read any of the body.
 *
 * esp_http_server ignores "Connection: close" in a response and, once the handler returns, reads
 * and discards what is left of the body (httpd_req_delete, CONFIG_HTTPD_PURGE_BUF_LEN = 32 B per
 * recv, each recv waiting up to recv_wait_timeout) on the server's only task: a refused 1.5 MB
 * upload held the web server for as long as the client kept sending, and anyone on the network
 * could repeat it. Here the session's receive function is replaced by one that fails.
 *
 * The session is always closed, in the same step: nothing of the body was read, so more than
 * CUT_MIN_BODY bytes remain; httpd_req_delete's purge goes past the few bytes httpd keeps from
 * the header parse (pending_data) and calls the receive function, which fails; the purge
 * returns an error and httpd_sess_process deletes the session (no later request can arrive on
 * it with the failing function). lwIP closes a socket with unread data with a reset, so a client
 * that is still sending may see the reset rather than the reply: acceptable for these.
 */
static void close_after_reply(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Connection", "close");
    if (req->content_len > CUT_MIN_BODY &&
        httpd_sess_set_recv_override(req->handle, httpd_req_to_sockfd(req), recv_refused) == ESP_OK) {
        atomic_fetch_add_explicit(&s_cut_bodies, 1u, memory_order_relaxed);
    }
}

/*
 * A refusal of an authenticated, same-origin request (400 bad image, 409 busy, 413, 415): the
 * body is not cut: it is drained in full after the reply, as before #106, so the web UI or a
 * script that is still sending gets the status and its reason instead of a reset (a cut under
 * a browser that is uploading 1.5 MB ends in a network error). The cost: an authenticated,
 * same-origin client holds the web server for as long as its upload keeps coming.
 */
static esp_err_t refuse_body(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_hdr(req, "Connection", "close");   /* the client may stop sending */
    esp_err_t ret = send_error(req, status, NULL, msg);
    /* Issue #110: the drain runs here rather than in httpd after we return, so that the stall
     * watchdog sees it: outside "in a handler", alive for every piece that arrives. A client
     * that stops sending ends it at the first receive timeout (5 s, under the watchdog's
     * limit); what is left is then cut, so httpd does not wait on it again. */
    web_guard_t *g = guard_of(req->handle);
    if (g != NULL) {
        atomic_store(&g->in_handler, false);
    }
    char sink[256];
    int r;
    while ((r = httpd_req_recv(req, sink, sizeof(sink))) > 0) {
        if (g != NULL) {
            atomic_store(&g->alive_ms, now_ms32());
        }
    }
    if (r < 0 && httpd_sess_set_recv_override(req->handle, httpd_req_to_sockfd(req),
                                              recv_refused) == ESP_OK) {
        atomic_fetch_add_explicit(&s_cut_bodies, 1u, memory_order_relaxed);
    }
    return ret;
}

/* Issue #64: the answer to a blocked address (no password check at all, right or wrong). */
static void send_blocked(httpd_req_t *req, uint32_t left)
{
    static char retry[12];   /* server task only; must outlive the response */
    snprintf(retry, sizeof(retry), "%lu", (unsigned long)((left + 999) / 1000));
    httpd_resp_set_hdr(req, "Retry-After", retry);
    close_after_reply(req);
    send_error(req, "429 Too Many Requests", NULL,
               "too many failed login attempts: wait and try again");
}

static bool authorized(httpd_req_t *req)
{
    char hdr[192];
    uint8_t ip[16];
    char who[48];
    peer_addr(req, ip, who, sizeof(who));
    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
    uint32_t left = gw_authlimit_check(&s_limit, ip, now);
    if (left != 0) {
        /* issue #64: a blocked address gets no password check at all, right or wrong */
        send_blocked(req, left);
        return false;
    }
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK) {
        /* PBKDF2 costs tens of ms: a browser repeats the same header on every request, so the
         * digest of the last accepted one is remembered. */
        uint8_t digest[GW_SHA256_LEN];
        gw_sha256(hdr, strlen(hdr), digest);
        if (s_ok_cached && gw_auth_equal(digest, s_ok_digest, sizeof(digest))) {
            gw_authlimit_succeeded(&s_limit, ip);
            return true;
        }
        if (check_credentials(hdr)) {
            memcpy(s_ok_digest, digest, sizeof(digest));
            s_ok_cached = true;
            gw_authlimit_succeeded(&s_limit, ip);
            return true;
        }
        uint32_t blocked = gw_authlimit_failed(&s_limit, ip, now);
        ESP_LOGW(TAG, "%s %s from %s: wrong credentials", req->method == HTTP_GET ? "GET" : "write",
                 req->uri, who);
        if (blocked != 0) {
            ESP_LOGW(TAG, "%s blocked for %lu s after %d failed logins in a row", who,
                     (unsigned long)(blocked / 1000), CONFIG_GW_WEB_LOGIN_MAX_FAILS);
        }
    }
    httpd_resp_set_hdr(req, "WWW-Authenticate", REALM_HDR);
    close_after_reply(req);
    send_error(req, "401 Unauthorized", NULL, "authentication required");
    return false;
}

/* Value of header @p name in @p buf ("" when absent). False if it does not fit. */
static bool header(httpd_req_t *req, const char *name, char *buf, size_t cap)
{
    buf[0] = '\0';
    size_t n = httpd_req_get_hdr_value_len(req, name);
    return n == 0 || (n < cap && httpd_req_get_hdr_value_str(req, name, buf, cap) == ESP_OK);
}

/* Issue #100: a state-changing request (PUT, POST, DELETE) must carry GW_CSRF_HEADER and, from a
 * browser, come from this same origin (gw_auth_csrf_check). Checked before the credentials:
 * a forged request is refused without a password check, so it neither succeeds with the
 * browser's cached credentials nor counts as a failed login against the operator's address. */
static bool same_origin(httpd_req_t *req)
{
    /* on the server task's stack, released before the handler validates anything */
    char host[96], origin[128], referer[256];
    bool fits = header(req, "Host", host, sizeof(host)) &&
                header(req, "Origin", origin, sizeof(origin)) &&
                header(req, "Referer", referer, sizeof(referer));
    bool marked = httpd_req_get_hdr_value_len(req, GW_CSRF_HEADER) > 0;
    gw_csrf_t r = !marked ? GW_CSRF_NO_HEADER
                  : !fits ? GW_CSRF_CROSS_ORIGIN
                          : gw_auth_csrf_check(s_https, host, origin, referer, "1");
    if (r == GW_CSRF_OK) {
        return true;
    }
    uint8_t ip[16];
    char who[48];
    peer_addr(req, ip, who, sizeof(who));
    s_csrf_refused++;
    ESP_LOGW(TAG, "%s %s from %s refused: %s",
             req->method == HTTP_PUT      ? "PUT"
             : req->method == HTTP_DELETE ? "DELETE"
                                          : "POST",
             req->uri, who,
             r == GW_CSRF_NO_HEADER ? "no " GW_CSRF_HEADER " header" : "cross-origin request");
    close_after_reply(req);   /* its body (an image?) is not read */
    send_error(req, "403 Forbidden", NULL,
               r == GW_CSRF_NO_HEADER
                   ? "missing header " GW_CSRF_HEADER ": writes need it (CSRF)"
                   : "cross-origin request refused (Origin or Referer is not this device)");
    return false;
}

static bool query_flag(httpd_req_t *req, const char *key)
{
    char q[64], v[8];
    /* issue #109 round 3b: exactly "1" (a stray value never turns a flag on, above all the
     * discard of an NVS migration copy) */
    return httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
           httpd_query_key_value(q, key, v, sizeof(v)) == ESP_OK && strcmp(v, "1") == 0;
}

/* ---------- CFG-3 / NET-5: the claim window (issue #120) ---------- */

/* The state is shared by the button task (open), the esp_timer task (end of the time) and the
 * server task (GET and POST /api/claim): under s_claim_mux, never held across a call. */
static portMUX_TYPE s_claim_mux = portMUX_INITIALIZER_UNLOCKED;
static gw_claim_t s_claim;
static esp_timer_handle_t s_claim_timer;
static atomic_bool s_pw_set;          /* s_hash is not "": the window never opens */
static atomic_uint s_claim_refused;
#if CONFIG_GW_IO_SIMULATED
static volatile uint32_t s_sim_claim_s;
static volatile bool s_sim_eth_as_ap;

void gw_web_sim_claim(uint32_t window_s, bool eth_as_ap)
{
    s_sim_claim_s = window_s;
    s_sim_eth_as_ap = eth_as_ap;
}
#endif

static uint64_t now_ms64(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static void claim_expired(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_claim_mux);
    bool was = s_claim.open;
    gw_claim_close(&s_claim);
    portEXIT_CRITICAL(&s_claim_mux);
    if (was) {
        ESP_LOGW(TAG, "claim window closed: its time is over (no web password was set)");
        if (s_ctx.claim_closed != NULL) {
            s_ctx.claim_closed(false);
        }
    }
}

uint32_t gw_web_claim_window_s(void)
{
    if (s_server == NULL || atomic_load(&s_pw_set)) {
        return 0;
    }
#if CONFIG_GW_IO_SIMULATED
    if (s_sim_claim_s != 0) {
        return s_sim_claim_s;
    }
#endif
    return CONFIG_GW_WEB_CLAIM_WINDOW_MIN * 60u;
}

bool gw_web_claim_open(uint32_t seconds)
{
    if (s_server == NULL || s_claim_timer == NULL) {
        return false;
    }
    portENTER_CRITICAL(&s_claim_mux);
    bool ok = gw_claim_open(&s_claim, atomic_load(&s_pw_set), seconds * 1000u, now_ms64());
    portEXIT_CRITICAL(&s_claim_mux);
    if (!ok) {
        ESP_LOGW(TAG, "claim window not opened: a web password is set (only a factory reset "
                      "removes it)");
        return false;
    }
    esp_timer_stop(s_claim_timer);
    esp_timer_start_once(s_claim_timer, (uint64_t)seconds * 1000000u);
    ESP_LOGW(TAG, "claim window open for %lu s: a client of the access point can set the web "
                  "password at /claim without a credential", (unsigned long)seconds);
    return true;
}

/* IPv4 address of @p a, also when it is mapped into IPv6; false for a real IPv6 address. */
static bool ipv4_of(const struct sockaddr_storage *a, uint8_t out[4])
{
    if (a->ss_family == AF_INET) {
        memcpy(out, &((const struct sockaddr_in *)a)->sin_addr, 4);
        return true;
    }
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    const uint8_t *v6 = (const uint8_t *)&((const struct sockaddr_in6 *)a)->sin6_addr;
    if (a->ss_family == AF_INET6 && memcmp(v6, mapped, sizeof(mapped)) == 0) {
        memcpy(out, v6 + 12, 4);
        return true;
    }
    return false;
}

/* The access point rule (gw_claim_from_ap) for the connection of @p req. An IPv6 connection never
 * passes: the access point has IPv4 only. */
static bool claim_from_ap(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_storage la, pa;
    socklen_t ln = sizeof(la), pn = sizeof(pa);
    uint8_t local[4], peer[4];
    if (getsockname(fd, (struct sockaddr *)&la, &ln) != 0 ||
        getpeername(fd, (struct sockaddr *)&pa, &pn) != 0 || !ipv4_of(&la, local) ||
        !ipv4_of(&pa, peer)) {
        return false;
    }
    gw_claim_if_t ap, others[GW_NET_IF_COUNT];
    int ap_if = GW_NET_IF_COUNT;   /* the access point is not an interface of gw_net */
    bool have_ap = s_ctx.ap_ip4 != NULL && s_ctx.ap_ip4(ap.ip, ap.mask);
#if CONFIG_GW_IO_SIMULATED
    if (s_sim_eth_as_ap) {
        ap_if = GW_NET_ETH;
        have_ap = gw_net_ip4_info(GW_NET_ETH, ap.ip, ap.mask);
    }
#endif
    if (!have_ap) {
        return false;
    }
    size_t n = 0;
    for (int i = 0; i < GW_NET_IF_COUNT; i++) {
        if (i != ap_if && gw_net_ip4_info((gw_net_if_t)i, others[n].ip, others[n].mask)) {
            n++;
        }
    }
    return gw_claim_from_ap(local, peer, &ap, others, n);
}

/* True while the window is open (and no password is set); @p left_ms gets the time left. */
static bool claim_window_open(uint32_t *left_ms)
{
    uint64_t now = now_ms64();
    portENTER_CRITICAL(&s_claim_mux);
    bool open = gw_claim_active(&s_claim, now) && !atomic_load(&s_pw_set);
    *left_ms = open ? gw_claim_left_ms(&s_claim, now) : 0;
    portEXIT_CRITICAL(&s_claim_mux);
    return open;
}

extern const char claim_html_start[] asm("_binary_claim_html_start");
extern const char claim_html_end[] asm("_binary_claim_html_end");

/* GET /claim: the page of the window. It holds nothing secret: no credential. */
static esp_err_t claim_page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, claim_html_start, claim_html_end - claim_html_start - 1);
}

/* GET /api/claim: {"open": the window is open, "allowed": this client may use it, "left_s",
 * "username": the user name for the new password, "" = any (recovery, commissioning)}. The user
 * name and the time go only to a client that may use the window. No credential. */
static esp_err_t claim_get(httpd_req_t *req)
{
    uint32_t left_ms;
    bool open = claim_window_open(&left_ms);
    bool allowed = open && claim_from_ap(req);
    char user[2 * GW_USER_SIZE], body[2 * GW_USER_SIZE + 96];
    json_escape(user, sizeof(user), allowed ? s_user : "");
    snprintf(body, sizeof(body),
             "{\"open\":%s,\"allowed\":%s,\"left_s\":%lu,\"username\":\"%s\"}\n",
             open ? "true" : "false", allowed ? "true" : "false",
             (unsigned long)(allowed ? (left_ms + 999) / 1000 : 0), user);
    return send_json(req, "200 OK", body);
}

/* POST /api/claim {"password": "..."}: the first web password, without a credential. The CSRF
 * check and the login block (#64) as for a login; a refusal because of the state (a password is
 * set, no window, not a client of the access point) counts as a failed login. */
static esp_err_t claim_post(httpd_req_t *req)
{
    if (!same_origin(req)) {
        return ESP_OK;
    }
    uint8_t ip[16];
    char who[48];
    peer_addr(req, ip, who, sizeof(who));
    uint64_t now = now_ms64();
    uint32_t left = gw_authlimit_check(&s_limit, ip, now);
    if (left != 0) {
        send_blocked(req, left);
        return ESP_OK;
    }
    char body[GW_CLAIM_BODY_MAX + 1];
    size_t got = 0;
    bool fits = req->content_len > 0 && req->content_len <= GW_CLAIM_BODY_MAX;
    int timeouts = 0;
    while (fits && got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) {
            continue;
        }
        if (r <= 0) {
            fits = false;
            break;
        }
        got += (size_t)r;
    }
    char pw[GW_CLAIM_PW_MAX + 1] = "";
    gw_claim_result_t parsed = fits ? gw_claim_parse(body, got, pw, sizeof(pw)) : GW_CLAIM_BAD_REQUEST;
    memset(body, 0, sizeof(body));
    bool from_ap = claim_from_ap(req);
    portENTER_CRITICAL(&s_claim_mux);
    gw_claim_result_t r = gw_claim_check(&s_claim, atomic_load(&s_pw_set), from_ap, pw, now);
    portEXIT_CRITICAL(&s_claim_mux);
    if (r != GW_CLAIM_PASSWORD_SET && r != GW_CLAIM_CLOSED && r != GW_CLAIM_NOT_AP &&
        parsed != GW_CLAIM_OK) {
        r = parsed;   /* the state allows a claim: the request itself is wrong */
    }
    if (!fits) {
        close_after_reply(req);   /* what is left of a long body is not read */
    }
    if (r == GW_CLAIM_PASSWORD_SET || r == GW_CLAIM_CLOSED || r == GW_CLAIM_NOT_AP) {
        memset(pw, 0, sizeof(pw));
        atomic_fetch_add(&s_claim_refused, 1u);
        uint32_t blocked = gw_authlimit_failed(&s_limit, ip, now);
        ESP_LOGW(TAG, "claim from %s refused: %s", who, gw_claim_text(r));
        if (blocked != 0) {
            ESP_LOGW(TAG, "%s blocked for %lu s after %d failed logins in a row", who,
                     (unsigned long)(blocked / 1000), CONFIG_GW_WEB_LOGIN_MAX_FAILS);
        }
        return send_error(req, "403 Forbidden", NULL, gw_claim_text(r));
    }
    if (r != GW_CLAIM_OK) {
        memset(pw, 0, sizeof(pw));
        return send_error(req, "400 Bad Request", NULL, gw_claim_text(r));
    }
    char hash[GW_PWHASH_SIZE];
    esp_err_t e = gw_storage_set_web_password(pw, hash, sizeof(hash));
    memset(pw, 0, sizeof(pw));
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "claim from %s: the web password was not stored (%s); the window stays open",
                 who, esp_err_to_name(e));
        return send_error(req, "503 Service Unavailable", NULL,
                          "the web password was not stored (NVS): the claim window stays open");
    }
    /* from now on only the new password opens the web UI: not the commissioning password, and
     * not a cached Authorization header */
    memcpy(s_hash, hash, sizeof(s_hash));
    memset(hash, 0, sizeof(hash));
    s_ok_cached = false;
    atomic_store(&s_pw_set, true);
    portENTER_CRITICAL(&s_claim_mux);
    gw_claim_done(&s_claim);
    portEXIT_CRITICAL(&s_claim_mux);
    esp_timer_stop(s_claim_timer);
    gw_authlimit_succeeded(&s_limit, ip);
    ESP_LOGW(TAG, "web password set through the claim window from %s: the window is closed and "
                  "does not open again", who);
    char user[2 * GW_USER_SIZE], answer[2 * GW_USER_SIZE + 48];
    json_escape(user, sizeof(user), s_user);
    snprintf(answer, sizeof(answer), "{\"ok\":true,\"username\":\"%s\"}\n", user);
    esp_err_t ret = send_json(req, "200 OK", answer);
    if (s_ctx.claim_closed != NULL) {
        s_ctx.claim_closed(true);
    }
    return ret;
}

/* ---------- /api/config ---------- */

static esp_err_t config_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    size_t len;
    gw_cfg_err_t err;
    char *doc = gw_storage_export_config(&len, &err);
    if (doc == NULL) {
        return send_error(req, "409 Conflict", &err, NULL);
    }
    if (query_flag(req, "download")) {
        httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"config.json\"");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_send(req, doc, (ssize_t)len);
    gw_storage_free(doc);
    return r;
}

/* Whole request body (NUL terminated), at most CONFIG_GW_CONFIG_MAX_BYTES. */
static char *read_body(httpd_req_t *req, size_t *len, const char **status)
{
    *status = NULL;
    if (req->content_len == 0) {
        *status = "400 Bad Request";
        return NULL;
    }
    if (req->content_len > CONFIG_GW_CONFIG_MAX_BYTES) {
        *status = "413 Content Too Large";
        return NULL;
    }
    char *buf = gw_storage_malloc(req->content_len + 1);
    if (buf == NULL) {
        *status = "503 Service Unavailable";
        return NULL;
    }
    size_t got = 0;
    int timeouts = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) {
            continue;
        }
        if (r <= 0) {
            heap_caps_free(buf);
            *status = "408 Request Timeout";
            return NULL;
        }
        got += (size_t)r;
    }
    buf[got] = '\0';
    *len = got;
    return buf;
}

/* #169: the warnings of an accepted document go into the answer as they come: the answer starts
 * at the first one (status 200, chunked), so nothing holds them in RAM. At most WEB_LINT_MAX;
 * "warning_count" gives all of them. */
#define WEB_LINT_MAX 50

typedef struct {
    httpd_req_t *req;
    bool dry_run;
    bool started;
    unsigned sent;
    esp_err_t err;
} lint_out_t;

static void lint_head(lint_out_t *o)
{
    httpd_resp_set_status(o->req, "200 OK");
    httpd_resp_set_type(o->req, "application/json");
    httpd_resp_set_hdr(o->req, "Cache-Control", "no-store");
    o->err = httpd_resp_sendstr_chunk(o->req, o->dry_run
                                                  ? "{\"ok\":true,\"saved\":false,\"warnings\":["
                                                  : "{\"ok\":true,\"saved\":true,\"restart\":true,"
                                                    "\"warnings\":[");
    o->started = true;
}

static bool lint_send(void *ctx, gw_lint_level_t level, const char *path, const char *msg)
{
    lint_out_t *o = ctx;
    (void)level;   /* a GW_LINT_ERROR never gets here: the validation refused it */
    if (!o->started) {
        lint_head(o);
    }
    char line[2 * GW_CFG_PATH_SIZE + 2 * GW_CFG_ERR_SIZE + 32];
    int n = snprintf(line, sizeof(line), "%s{\"path\":\"", o->sent > 0 ? "," : "");
    n += (int)json_escape(line + n, sizeof(line) - (size_t)n, path);
    n += snprintf(line + n, sizeof(line) - (size_t)n, "\",\"message\":\"");
    n += (int)json_escape(line + n, sizeof(line) - (size_t)n, msg);
    snprintf(line + n, sizeof(line) - (size_t)n, "\"}");
    if (o->err == ESP_OK) {
        o->err = httpd_resp_sendstr_chunk(o->req, line);
    }
    return ++o->sent < WEB_LINT_MAX && o->err == ESP_OK;
}

/* Issue #146: POST /api/config?restore_prev=1 (no body): the button "Restore it" of the web UI in
 * recovery mode. config.prev.json is validated as at the boot (with the stored secrets) and
 * becomes config.json; then the gateway restarts. 404 when there is none, 400 when this firmware
 * rejects it, 409 during the trial of a new image. */
static esp_err_t config_restore_prev(httpd_req_t *req)
{
    gw_cfg_err_t err;
    esp_err_t r = gw_storage_cfgprev_restore_checked(&err);
    switch (r) {
    case ESP_OK:
        break;
    case ESP_ERR_NOT_FOUND:
        return send_error(req, "404 Not Found", &err, NULL);
    case ESP_ERR_INVALID_ARG:
        ESP_LOGW(TAG, "configuration from before the update rejected: %s", err.msg);
        return send_error(req, "400 Bad Request", &err, NULL);
    case ESP_ERR_INVALID_STATE:
        return send_error(req, "409 Conflict", &err, NULL);
    case ESP_ERR_NO_MEM:
        return send_error(req, "503 Service Unavailable", &err, NULL);
    default:
        return send_error(req, "500 Internal Server Error", &err, NULL);
    }
    send_json(req, "200 OK", "{\"ok\":true,\"restart\":true}\n");
    ESP_LOGW(TAG, "configuration from before the update restored through the REST API: "
                  "restarting");
    s_ctx.restart();
    return ESP_OK;
}

static esp_err_t config_put(httpd_req_t *req)
{
    if (!same_origin(req) || !authorized(req)) {
        return ESP_OK;
    }
    if (query_flag(req, "restore_prev")) {
        return config_restore_prev(req);
    }
    size_t len;
    const char *status;
    char *body = read_body(req, &len, &status);
    if (body == NULL) {
        char msg[96];
        snprintf(msg, sizeof(msg), "body of %u bytes not accepted (maximum %d)",
                 (unsigned)req->content_len, CONFIG_GW_CONFIG_MAX_BYTES);
        return refuse_body(req, status, msg);
    }
    bool dry_run = query_flag(req, "dry_run");
    /* issue #109: ?discard_nvs_copy=1, the explicit way out when LittleFS may hold an unreachable
     * NVS migration copy (README 6.2.1): formats LittleFS, the copy is lost */
    bool discard = query_flag(req, "discard_nvs_copy");
    gw_cfg_err_t err;
    lint_out_t lo = {.req = req, .dry_run = dry_run};
    size_t warnings = 0;
    esp_err_t r = gw_storage_save_config_lint(body, len, dry_run, discard, &err, lint_send, &lo,
                                              &warnings);
    memset(body, 0, len);   /* may hold secrets in clear */
    heap_caps_free(body);
    switch (r) {
    case ESP_OK:
        break;
    case ESP_ERR_INVALID_ARG:
        ESP_LOGW(TAG, "configuration rejected: %s", err.msg);
        return send_error(req, "400 Bad Request", &err, NULL);
    case ESP_ERR_INVALID_SIZE:
        return send_error(req, "413 Content Too Large", &err, NULL);
    case ESP_ERR_NO_MEM:
        return send_error(req, "503 Service Unavailable", &err, NULL);
    case ESP_ERR_INVALID_STATE:
        /* issue #144 item 1: during the trial of a new image ("testing <v>: N s left") */
        ESP_LOGW(TAG, "configuration refused: %s", err.msg);
        return send_error(req, "409 Conflict", &err, NULL);
    default:
        return send_error(req, "500 Internal Server Error", &err, NULL);
    }
    if (!lo.started) {
        lint_head(&lo);
    }
    char tail[48];
    snprintf(tail, sizeof(tail), "],\"warning_count\":%u}\n", (unsigned)warnings);
    if (lo.err == ESP_OK) {
        lo.err = httpd_resp_sendstr_chunk(req, tail);
    }
    if (lo.err == ESP_OK) {
        lo.err = httpd_resp_send_chunk(req, NULL, 0);
    }
    if (dry_run) {
        return lo.err;
    }
    ESP_LOGW(TAG, "new configuration saved through the REST API: restarting");
    s_ctx.restart();
    return ESP_OK;
}

static esp_err_t restart_post(httpd_req_t *req)
{
    if (!same_origin(req) || !authorized(req)) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "restart requested through the REST API");
    send_json(req, "202 Accepted", "{\"ok\":true,\"restart\":true}\n");
    s_ctx.restart();
    return ESP_OK;
}

/* ---------- #171: POST /api/rebirth ---------- */

/* The NBIRTH and every DBIRTH again, with the same bdSeq, as an NCMD Node Control/Rebirth (and
 * the shell's rebirth): for a host that shows the tags as alias_<n> after its own restart and
 * does not ask for a rebirth. Same checks as every write (CSRF, then the password). */
static esp_err_t rebirth_post(httpd_req_t *req)
{
    if (!same_origin(req) || !authorized(req)) {
        return ESP_OK;
    }
    if (s_ctx.mode != GW_WEB_NORMAL || !gw_edge_request_rebirth()) {
        return refuse_body(req, "409 Conflict",
                           s_ctx.mode == GW_WEB_FAULT
                               ? "no Sparkplug session: the gateway does not start (hardware "
                                 "fault, see the status)"
                           : s_ctx.mode != GW_WEB_NORMAL
                               ? "no Sparkplug session: the gateway has no valid configuration"
                               : "no Sparkplug session: the gateway is not connected to the broker");
    }
    ESP_LOGI(TAG, "rebirth requested through the REST API");
    return send_json(req, "202 Accepted", "{\"ok\":true,\"rebirth\":true}\n");
}

/* ---------- #160: POST /api/time ---------- */

/* TIME-1 / issue #160: the web UI's button "Set the time from the browser", for a network with
 * no time server and a gateway with no RTC time. {"utc_ms": <ms UTC>}: the gateway sets its
 * clock and the RTC; it publishes NDATA and DDATA again, after a rebirth if the births went out
 * without a valid clock. Same checks as every write (CSRF, then the password); in every mode
 * (the RTC keeps the time for the next boot). Not over a clock that SNTP synchronises. */
static esp_err_t time_post(httpd_req_t *req)
{
    if (!same_origin(req) || !authorized(req)) {
        return ESP_OK;
    }
    size_t len;
    const char *status;
    char *body = req->content_len <= 128 ? read_body(req, &len, &status) : NULL;
    if (body == NULL) {
        return refuse_body(req, "400 Bad Request",
                           "expected {\"utc_ms\": <milliseconds since 1970-01-01T00:00:00Z>} (at "
                           "most 128 bytes)");
    }
    int64_t ms = 0;
    char why[120];
    gw_clock_set_t r = gw_clock_parse_set(body, len, gw_edge_time_source(), &ms, why, sizeof(why));
    heap_caps_free(body);
    if (r == GW_CLOCK_SET_OK && !gw_edge_set_browser_time(ms)) {
        r = GW_CLOCK_SET_SYNCED;   /* SNTP answered in between */
        snprintf(why, sizeof(why), "SNTP synchronises the clock: the time of the browser is not used");
    }
    if (r != GW_CLOCK_SET_OK) {
        return send_error(req, r == GW_CLOCK_SET_SYNCED ? "409 Conflict" : "400 Bad Request", NULL,
                          why);
    }
    ESP_LOGI(TAG, "clock set through the REST API");
    char answer[96];
    snprintf(answer, sizeof(answer), "{\"ok\":true,\"utc_ms\":%lld,\"source\":\"browser\"}\n",
             (long long)ms);
    return send_json(req, "200 OK", answer);
}

/* ---------- #182: POST /api/modbus/read ---------- */

_Static_assert(GW_MBDIAG_MAX == GW_BUS_DIAG_MAX, "one test read is one diagnostic read of the bus");

/* A test read for commissioning (is a slave there, what does it answer), through the bus owner
 * as the shell's mbread (MB-16): the bus task runs it between two poll transactions, from its
 * static request slot (no allocation in the bus path); this task waits for the answer. */
static esp_err_t modbus_read_post(httpd_req_t *req)
{
    if (!same_origin(req) || !authorized(req)) {
        return ESP_OK;
    }
    if (s_ctx.mode != GW_WEB_NORMAL) {
        return refuse_body(req, "409 Conflict",
                           s_ctx.mode == GW_WEB_RECOVERY
                               ? "the RS485 bus does not run: the configuration was rejected "
                                 "(recovery mode). Correct it and save it first"
                           : s_ctx.mode == GW_WEB_FAULT
                               ? "the RS485 bus does not run: the gateway does not start "
                                 "(hardware fault, see the status)"
                               : "the RS485 bus does not run: there is no configuration yet "
                                 "(commissioning). Save a configuration first");
    }
    size_t len;
    const char *status;
    char *body = req->content_len <= 512 ? read_body(req, &len, &status) : NULL;
    if (body == NULL) {
        return refuse_body(req, "400 Bad Request",
                           "expected {\"unit\": 1, \"area\": \"holding\", \"address\": 0, "
                           "\"count\": 1} (at most 512 bytes)");
    }
    gw_mbdiag_req_t rq;
    char why[160];
    bool parsed = gw_mbdiag_parse(body, len, &rq, why, sizeof(why));
    heap_caps_free(body);
    if (!parsed) {
        return send_error(req, "400 Bad Request", NULL, why);
    }
    uint16_t v[GW_BUS_DIAG_MAX];
    uint8_t exc;
    /* worst case: a poll cycle in progress plus every retry of this request (as mbread) */
    uint32_t wait = 2000u + (uint32_t)s_ctx.cfg->rs485.response_timeout_ms *
                                (s_ctx.cfg->rs485.retries + 1u) * 2u;
    mb_status_t st = gw_bus_diag_read(rq.unit, gw_mbdiag_fc(&rq), rq.address, rq.count, v, &exc,
                                      wait);
    char answer[512];
    const char *http = gw_mbdiag_answer(&rq, st, exc, v, answer, sizeof(answer));
    ESP_LOGI(TAG, "Modbus test read through the REST API: unit %u FC%02u %u x%u: %s", rq.unit,
             (unsigned)gw_mbdiag_fc(&rq), rq.address, rq.count, http);
    return send_json(req, http, answer);
}

/* ---------- #139: POST /api/card/format?confirm=1 ---------- */

/* The UI's text for the answers that are not "started" (the shell has them in English). */
static const char *card_answer_text(gw_cardfmt_answer_t a)
{
    switch (a) {
    case GW_CARDFMT_BUSY:
        return "a format is already in progress";
    case GW_CARDFMT_NO_SUPPORT:
        return "this build has no microSD card (QEMU): nothing was formatted";
    case GW_CARDFMT_NO_PSRAM:
        return "the format needs PSRAM and this device has none: nothing was formatted";
    default:
        return "no memory for the format task: nothing was formatted";
    }
}

/* Same checks as every write (CSRF, then the password); the request must say ?confirm=1 (the
 * web UI asks the operator to type a word first). The format runs in its own task: the answer
 * comes at once, and /api/status shows the progress and the result. */
static esp_err_t card_format_post(httpd_req_t *req)
{
    if (!same_origin(req) || !authorized(req)) {
        return ESP_OK;
    }
    if (!query_flag(req, "confirm")) {
        return send_error(req, "400 Bad Request", NULL,
                          "missing ?confirm=1: the format erases all the contents of the card");
    }
    gw_cardfmt_answer_t a = gw_sfcard_format_start();
    if (a == GW_CARDFMT_STARTED) {
        ESP_LOGW(TAG, "microSD card format requested through the REST API");
        return send_json(req, "202 Accepted", "{\"ok\":true,\"started\":true}\n");
    }
    char body[200];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\",\"reason\":\"%s\"}\n",
             card_answer_text(a), gw_cardfmt_answer_key(a));
    return send_json(req, gw_cardfmt_answer_http(a), body);
}

/* ---------- FW-1: /api/firmware ---------- */

static const char *const k_fw_phase[] = {"idle", "downloading", "restarting", "failed",
                                         "testing", "installed", "rolled_back"};
static const char *const k_fw_slot[] = {"empty", "previous", "new", "rejected", "invalid"};

static esp_err_t firmware_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    /* about 3 KB for one request: from the PSRAM (gw_storage_malloc), not the server task's
     * stack nor .bss (issue #80: internal RAM is shared with Wi-Fi, lwIP and the stacks) */
    typedef struct {
        gw_fw_status_t f;
        char text[2 * sizeof(((gw_fw_status_t *)0)->text)];
        char detail[2 * sizeof(((gw_fw_status_t *)0)->detail)];
        char run[2 * sizeof(((gw_fw_status_t *)0)->running)];
        char other[2 * sizeof(((gw_fw_status_t *)0)->other)];
        char other_why[2 * sizeof(((gw_fw_status_t *)0)->other_why)];
        gw_cfgprev_t prev;
        char prev_ver[2 * GW_CFGPREV_VERSION];
        char body[2000];
    } fw_json_t;
    fw_json_t *j = gw_storage_malloc(sizeof(*j));
    if (j == NULL) {
        return send_error(req, "503 Service Unavailable", NULL, "no memory");
    }
    gw_fw_get_status(&j->f);
    json_escape(j->text, sizeof(j->text), j->f.text);
    json_escape(j->detail, sizeof(j->detail), j->f.detail);
    json_escape(j->run, sizeof(j->run), j->f.running);
    json_escape(j->other, sizeof(j->other), j->f.other);
    json_escape(j->other_why, sizeof(j->other_why), j->f.other_why);
    /* issue #146: the configuration from before the update; the web UI restores it in recovery
     * mode (POST /api/config?restore_prev=1) */
    bool prev = gw_storage_cfgprev_get(&j->prev) && j->prev.version[0] != '\0';
    json_escape(j->prev_ver, sizeof(j->prev_ver), prev ? j->prev.version : "");
    /* issue #144: both slots, the trial (expected mode, time left), the boot-loop counter */
    bool can_boot = j->f.enabled && !j->f.trial && j->f.phase != GW_FW_DOWNLOADING &&
                    j->f.phase != GW_FW_RESTARTING && j->f.other_state != GW_FW_SLOT_EMPTY &&
                    j->f.other_state != GW_FW_SLOT_INVALID;
    snprintf(j->body, sizeof(j->body),
             "{\"running\":\"%s\",\"slot\":\"%s\",\"other\":\"%s\",\"other_slot\":\"%s\","
             "\"other_state\":\"%s\",\"other_why\":\"%s\",\"can_boot_other\":%s,"
             "\"enabled\":%s,\"phase\":\"%s\",\"state\":\"%s\",\"detail\":\"%s\","
             "\"progress\":%u,\"bytes\":%lu,\"total\":%lu,\"trial\":%s,"
             "\"trial_left_s\":%lu,\"expected_mode\":\"%s\",\"boot_count\":%lu,"
             "\"safe_mode\":%s,\"cfg_prev\":{\"version\":\"%s\",\"saved\":%lld,"
             "\"schema\":%u},\"cfg_prev_restore\":%s}\n",
             j->run, j->f.slot, j->other, j->f.other_slot,
             j->f.other_state < sizeof(k_fw_slot) / sizeof(k_fw_slot[0])
                 ? k_fw_slot[j->f.other_state] : "?",
             j->other_why, can_boot ? "true" : "false", j->f.enabled ? "true" : "false",
             j->f.phase < sizeof(k_fw_phase) / sizeof(k_fw_phase[0]) ? k_fw_phase[j->f.phase] : "?",
             j->text, j->detail, j->f.progress, (unsigned long)j->f.bytes,
             (unsigned long)j->f.total, j->f.trial ? "true" : "false",
             (unsigned long)j->f.trial_left_s,
             j->f.trial ? gw_ota_mode_name(j->f.expected) : "",
             (unsigned long)j->f.boot_count, j->f.safe_mode ? "true" : "false", j->prev_ver,
             prev ? (long long)j->prev.saved : 0LL, prev ? (unsigned)j->prev.schema : 0u,
             prev && s_ctx.mode == GW_WEB_RECOVERY ? "true" : "false");
    esp_err_t r = send_json(req, "200 OK", j->body);
    heap_caps_free(j);
    return r;
}

/* "application/json" or "application/octet-stream", with or without parameters. */
static bool media_type_is(const char *ctype, const char *type)
{
    size_t n = strlen(type);
    return strncasecmp(ctype, type, n) == 0 && (ctype[n] == '\0' || ctype[n] == ';' || ctype[n] == ' ');
}

/* {"url": "https://..."} (application/json): the gateway downloads it.
 * application/octet-stream: the body is the image itself, written as it arrives. Any other
 * content type is refused (issue #100: text/plain is what a foreign page can send).
 * Issue #144 item 3: "force": true in the JSON, or ?force=1 with an image, installs an image
 * that the pre-flight check refuses (409 without it); it must then reach recovery mode. */
static esp_err_t firmware_post(httpd_req_t *req)
{
    if (!same_origin(req) || !authorized(req)) {
        return ESP_OK;
    }
    char ctype[48] = "";
    httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof(ctype));
    char why[320];   /* issue #144: the reasons of the pre-flight check */
    bool json = media_type_is(ctype, "application/json");
    if (!json && !media_type_is(ctype, "application/octet-stream")) {
        return refuse_body(req, "415 Unsupported Media Type",
                           "Content-Type: application/json ({\"url\": ...}) or "
                           "application/octet-stream (the image)");
    }
    if (json) {
        size_t len;
        const char *status;
        char *body = req->content_len <= 1024 ? read_body(req, &len, &status) : NULL;
        if (body == NULL) {
            return refuse_body(req, "400 Bad Request", "expected {\"url\": \"https://...\"}");
        }
        char url[GW_OTA_URL_MAX];
        bool force = false;
        bool parsed = gw_ota_parse_request(body, len, url, &force, why, sizeof(why));
        heap_caps_free(body);
        if (!parsed) {
            return send_error(req, "400 Bad Request", NULL, why);
        }
        esp_err_t e = gw_fw_start_url(url, force, why, sizeof(why));
        if (e != ESP_OK) {
            return send_error(req, e == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "409 Conflict",
                              NULL, why);
        }
        ESP_LOGW(TAG, "firmware update from %s requested through the REST API", url);
        return send_json(req, "202 Accepted", "{\"ok\":true,\"started\":true}\n");
    }
    if (req->content_len == 0) {
        return send_error(req, "411 Length Required", NULL, "image without Content-Length");
    }
    esp_err_t e = gw_fw_upload_begin(req->content_len, query_flag(req, "force"), why,
                                     sizeof(why));
    if (e != ESP_OK) {
        return refuse_body(req, e == ESP_ERR_INVALID_SIZE ? "413 Content Too Large" : "409 Conflict",
                           why);
    }
    ESP_LOGW(TAG, "firmware upload of %u bytes through the REST API", (unsigned)req->content_len);
    const size_t chunk = 4096;
    char *buf = gw_storage_malloc(chunk);
    size_t got = 0;
    int timeouts = 0;
    bool ok = buf != NULL;
    while (ok && got < req->content_len) {
        size_t want = req->content_len - got < chunk ? req->content_len - got : chunk;
        int r = httpd_req_recv(req, buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) {
            continue;
        }
        if (r <= 0) {
            break;
        }
        timeouts = 0;
        esp_err_t w = gw_fw_upload_write(buf, (size_t)r, why, sizeof(why));
        if (w != ESP_OK) {
            heap_caps_free(buf);
            /* issue #144 item 3: the pre-flight check refused (force with ?force=1) */
            return refuse_body(req, w == ESP_ERR_NOT_SUPPORTED ? "409 Conflict" : "400 Bad Request",
                               why);
        }
        got += (size_t)r;
    }
    heap_caps_free(buf);
    e = gw_fw_upload_end(ok && got == req->content_len, why, sizeof(why));
    if (e != ESP_OK) {
        return refuse_body(req, !ok ? "503 Service Unavailable"
                                : got < req->content_len ? "408 Request Timeout" : "400 Bad Request",
                           why);
    }
    return send_json(req, "200 OK", "{\"ok\":true,\"restart\":true}\n");
}

/* Issue #144 item 6: POST /api/firmware/other: the "Restart into <version>" button. The image
 * of the other slot boots on trial; refused during a trial (409), when the slot is empty or
 * invalid (404), or when the pre-flight check refuses it (409, ?force=1 forces it). */
static esp_err_t firmware_other_post(httpd_req_t *req)
{
    if (!same_origin(req) || !authorized(req)) {
        return ESP_OK;
    }
    char why[320];   /* issue #146: also the JSON paths that changed */
    esp_err_t e = gw_fw_boot_other(query_flag(req, "force"), why, sizeof(why));
    if (e == ESP_ERR_NOT_FOUND) {
        return refuse_body(req, "404 Not Found", why);
    }
    if (e != ESP_OK) {
        return refuse_body(req, "409 Conflict", why);
    }
    ESP_LOGW(TAG, "boot of the other slot requested through the REST API");
    return send_json(req, "202 Accepted", "{\"ok\":true,\"restart\":true}\n");
}

/* ---------- #132: /api/coredump ---------- */

/* The raw dump (header, ELF and checksum: esp-coredump --core-format raw), streamed from the
 * partition in pieces: nothing of the dump's size is held. The dump is a copy of the RAM at the
 * crash and can hold secrets in clear text: the password is needed in every mode (recovery
 * too), and the piece buffer is cleared before it is freed. */
static esp_err_t coredump_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    uint32_t size;
    if (gw_coredump_state(&size) == GW_CDUMP_NONE) {
        return send_error(req, "404 Not Found", NULL, "no core dump in flash");
    }
    if (size == 0) {
        return send_error(req, "409 Conflict", NULL,
                          "the core dump partition holds no readable dump (its size is not "
                          "valid): erase it with DELETE /api/coredump");
    }
    const size_t chunk = 2048;
    char *buf = gw_storage_malloc(chunk);
    if (buf == NULL) {
        return send_error(req, "503 Service Unavailable", NULL, "no memory");
    }
    /* server task only; must outlive the response */
    static char disp[48 + sizeof(s_ctx.cfg->network.hostname) + 32];
    snprintf(disp, sizeof(disp), "attachment; filename=\"coredump-%s-%.32s.bin\"",
             s_ctx.cfg->network.hostname, s_ctx.info->firmware);
    esp_err_t r = ESP_OK;
    uint32_t off = 0;
    while (off < size && r == ESP_OK) {
        size_t n = size - off < chunk ? size - off : chunk;
        esp_err_t e = gw_coredump_read(off, buf, n);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "core dump download stopped at byte %lu: %s", (unsigned long)off,
                     esp_err_to_name(e));
            if (off == 0) {   /* nothing sent yet: a clean answer */
                memset(buf, 0, chunk);
                heap_caps_free(buf);
                return send_error(req, "404 Not Found", NULL, "the core dump was erased");
            }
            r = ESP_FAIL;   /* the session closes: the client sees an incomplete transfer */
            break;
        }
        if (off == 0) {
            httpd_resp_set_type(req, "application/octet-stream");
            httpd_resp_set_hdr(req, "Content-Disposition", disp);
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        }
        r = httpd_resp_send_chunk(req, buf, (ssize_t)n);
        off += (uint32_t)n;
    }
    memset(buf, 0, chunk);
    heap_caps_free(buf);
    if (r == ESP_OK) {
        r = httpd_resp_send_chunk(req, NULL, 0);
    }
    if (r == ESP_OK) {
        ESP_LOGI(TAG, "core dump of %lu bytes downloaded through the REST API",
                 (unsigned long)size);
    }
    return r;
}

/* Erase the dump: after a confirmed download, or without one. Same checks as every write
 * (CSRF, then the password). */
static esp_err_t coredump_delete(httpd_req_t *req)
{
    if (!same_origin(req) || !authorized(req)) {
        return ESP_OK;
    }
    if (!gw_coredump_present()) {
        return send_error(req, "404 Not Found", NULL, "no core dump in flash");
    }
    esp_err_t e = gw_coredump_erase("REST API");
    if (e != ESP_OK) {
        return send_error(req, "500 Internal Server Error", NULL, esp_err_to_name(e));
    }
    gw_edge_notify_core_dump();   /* NDATA Node Info/Core Dump = false */
    return send_json(req, "200 OK", "{\"ok\":true,\"erased\":true}\n");
}

/* The summary (task, PC, backtrace, panic reason, and the reset reason of this boot). It reads
 * the dump, so it is a route of its own and not part of /api/status, which the web UI polls. */
static esp_err_t coredump_summary_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    typedef struct {
        gw_cdump_info_t i;
        char body[900];
    } summary_json_t;
    summary_json_t *j = gw_storage_malloc(sizeof(*j));
    if (j == NULL) {
        return send_error(req, "503 Service Unavailable", NULL, "no memory");
    }
    esp_err_t r;
    if (gw_coredump_summary(&j->i) != ESP_OK ||
        gw_cdump_summary_json(&j->i, s_ctx.info->reset_reason, j->body, sizeof(j->body)) == 0) {
        r = send_error(req, "503 Service Unavailable", NULL, "no memory for the summary");
    } else {
        r = send_json(req, "200 OK", j->body);
    }
    heap_caps_free(j);
    return r;
}

/* ---------- / and /api/status ---------- */

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static esp_err_t index_get(httpd_req_t *req)
{
    /* #120: a browser without credentials, on the access point while the claim window is open,
     * goes to the page of the window instead of the login prompt */
    uint32_t left_ms;
    if (httpd_req_get_hdr_value_len(req, "Authorization") == 0 && claim_window_open(&left_ms) &&
        claim_from_ap(req)) {
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/claim");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, NULL, 0);
    }
    if (!authorized(req)) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /* EMBED_TXTFILES appends a NUL */
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

/* Chunked JSON writer: a small buffer flushed as HTTP chunks (256 tags do not fit in RAM
 * twice, and nothing here allocates). */
typedef struct {
    httpd_req_t *req;
    char buf[1024];
    size_t len;
    esp_err_t err;
} out_t;

static void out_flush(out_t *o)
{
    if (o->len > 0 && o->err == ESP_OK) {
        o->err = httpd_resp_send_chunk(o->req, o->buf, (ssize_t)o->len);
    }
    o->len = 0;
}

static void outf(out_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void outf(out_t *o, const char *fmt, ...)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(o->buf + o->len, sizeof(o->buf) - o->len, fmt, ap);
        va_end(ap);
        if (n >= 0 && (size_t)n < sizeof(o->buf) - o->len) {
            o->len += (size_t)n;
            return;
        }
        out_flush(o);   /* did not fit: flush and write again into the empty buffer */
    }
}

/* Quoted JSON string. */
static void out_str(out_t *o, const char *s)
{
    char e[2 * GW_CFG_ERR_SIZE];
    json_escape(e, sizeof(e), s);
    outf(o, "\"%s\"", e);
}

static gw_tag_value_t s_vals[GW_MAX_TAGS_PER_DEVICE];   /* server task only */

/* "off: why" (the shell's spill line) without its "off: " */
static const char *spill_why(const char *s)
{
    return strncmp(s, "off: ", 5) == 0 ? s + 5 : s;
}

/* #139: the card, the spill and the format, as optional objects (older clients ignore them).
 * "spill" only with a configuration (normal mode). Not inlined: its buffers stay off the
 * stack frame of status_get. */
static __attribute__((noinline)) void status_card(out_t *o, const gw_config_t *c)
{
    char buf[400];
    gw_card_info_t ci;
    gw_sfcard_get_info(&ci);
    gw_card_json(&ci, buf, sizeof(buf));
    outf(o, ",\"card\":%s", buf);
    gw_cardfmt_t f;
    gw_sfcard_get_format(&f);
    gw_cardfmt_json(&f, now_ms32(), buf, sizeof(buf));
    outf(o, ",\"card_format\":%s", buf);
    if (s_ctx.mode != GW_WEB_NORMAL) {
        return;
    }
    gw_edge_status_t es;
    gw_edge_get_status(&es);
    if (!c->sparkplug.store_forward || !c->sparkplug.sf_sd) {
        outf(o, ",\"spill\":{\"state\":\"off\",\"detail\":\"%s\"}",
             c->sparkplug.store_forward ? "store_forward.sd is off" : "store_forward is off");
        return;
    }
    if (!es.spill_on) {
        outf(o, ",\"spill\":{\"state\":\"not_started\",\"detail\":");
        out_str(o, spill_why(es.spill_off));
        outf(o, "}");
        return;
    }
    if (es.spill_failed) {
        outf(o, ",\"spill\":{\"state\":\"%s\",\"detail\":",
             es.spill_released ? "stopped" : "failed");
        out_str(o, es.spill_off);
        outf(o, ",\"stranded\":%lu,\"moved\":%lu}", (unsigned long)es.spill_stranded,
             (unsigned long)es.spilled);
        return;
    }
    outf(o,
         ",\"spill\":{\"state\":\"on\",\"ready\":%s,\"waiting\":%lu,"
         "\"moved\":%lu,\"overwritten\":%lu,\"holes\":%lu,\"ring_blocks\":%lu,"
         "\"waiting_blocks\":%lu,\"extents\":%lu,\"kb\":%lu,\"blocks_written\":%lu,"
         "\"checkpoints\":%lu,\"boot_ms\":%lu,\"files_to_delete\":%lu}",
         es.spill_ready ? "true" : "false", (unsigned long)es.spill_count,
         (unsigned long)es.spilled,
         (unsigned long)es.spill_dropped, (unsigned long)es.spill_holes,
         (unsigned long)es.spill_nblocks, (unsigned long)es.spill_waiting_blocks,
         (unsigned long)es.spill_extents, (unsigned long)es.spill_kb,
         (unsigned long)es.spill_blocks, (unsigned long)es.spill_checkpoints,
         (unsigned long)es.spill_boot_ms, (unsigned long)es.spill_gc_pending);
}

static esp_err_t status_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    static const char *const modes[] = {"normal", "recovery", "commissioning", "fault"};
    static out_t o;   /* server task only */
    o.req = req;
    o.len = 0;
    o.err = ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const gw_config_t *c = s_ctx.cfg;
    uint32_t f = gw_status_get();
    outf(&o, "{\"mode\":\"%s\",\"fault\":", modes[s_ctx.mode]);
    out_str(&o, s_ctx.fault != NULL ? s_ctx.fault : "");   /* #135 */
    outf(&o, ",\"firmware\":");
    out_str(&o, s_ctx.info->firmware);
    outf(&o, ",\"reset_reason\":");
    out_str(&o, s_ctx.info->reset_reason);
    outf(&o, ",\"uptime_s\":%lld,\"config\":{\"ok\":%s,\"source\":",
         esp_timer_get_time() / 1000000, s_ctx.mode == GW_WEB_NORMAL ? "true" : "false");
    out_str(&o, s_ctx.config_source);
    outf(&o, ",\"error\":");
    out_str(&o, s_ctx.config_error != NULL ? s_ctx.config_error->msg : "");
    outf(&o, ",\"path\":");
    out_str(&o, s_ctx.config_error != NULL ? s_ctx.config_error->path : "");
    outf(&o, "},\"network\":{\"up\":%s,\"interfaces\":[", (f & GW_ST_NET_UP) ? "true" : "false");
    gw_net_if_status_t ifs[GW_NET_IF_COUNT];
    gw_net_get_status(ifs);
    static const char *const names[GW_NET_IF_COUNT] = {"ethernet", "wifi"};
    for (int i = 0; i < GW_NET_IF_COUNT; i++) {
        const gw_net_if_status_t *n = &ifs[i];
        outf(&o,
             "%s{\"name\":\"%s\",\"configured\":%s,\"link\":%s,\"usable\":%s,\"active\":%s,"
             "\"ipv4\":\"",
             i ? "," : "", names[i], n->configured ? "true" : "false", n->link ? "true" : "false",
             n->usable ? "true" : "false", n->active ? "true" : "false");
        if (n->ipv4[0] | n->ipv4[1] | n->ipv4[2] | n->ipv4[3]) {
            outf(&o, "%u.%u.%u.%u", n->ipv4[0], n->ipv4[1], n->ipv4[2], n->ipv4[3]);
        }
        outf(&o, "\",\"ipv6\":[");
        bool first = true;
        for (int k = 0; k < GW_NET_IPV6_MAX; k++) {
            if (n->ipv6[k][0] != '\0') {
                outf(&o, "%s\"%s\"", first ? "" : ",", n->ipv6[k]);
                first = false;
            }
        }
        outf(&o, "]}");
    }
    outf(&o, "]},\"mqtt\":{\"connected\":%s}", (f & GW_ST_MQTT_UP) ? "true" : "false");
    if (s_ctx.mode == GW_WEB_NORMAL) {
        outf(&o, ",\"sparkplug\":{\"group_id\":");
        out_str(&o, c->sparkplug.group_id);
        outf(&o, ",\"edge_node_id\":");
        out_str(&o, c->sparkplug.edge_node_id);
        outf(&o, ",\"diagnostics_s\":%lu}", (unsigned long)c->sparkplug.diagnostics_s);
        gw_log_stats_t ls;
        gw_log_get_stats(&ls);
        if (ls.enabled) {
            char line[200];
            snprintf(line, sizeof(line), "%s:%u %s, sent %lu, dropped %lu, errors %lu",
                     c->syslog.host, c->syslog.port, ls.connected ? "connected" : "not reachable",
                     (unsigned long)ls.sent, (unsigned long)ls.dropped,
                     (unsigned long)ls.send_errors);
            outf(&o, ",\"syslog\":");
            out_str(&o, line);
        }
    }
    outf(&o, ",\"devices\":[");
    for (uint16_t d = 0; s_ctx.mode == GW_WEB_NORMAL && d < c->device_count; d++) {
        const gw_device_cfg_t *dev = &c->devices[d];
        bool online;
        gw_dev_stats_t st;
        gw_tagdb_get_device(c, d, s_vals, &online);
        gw_tagdb_get_stats(d, &st);
        outf(&o, "%s{\"name\":", d ? "," : "");
        out_str(&o, dev->name);
        outf(&o,
             ",\"unit_id\":%u,\"online\":%s,\"cycles\":%lu,\"failed\":%lu,\"ok\":%lu,"
             "\"timeout\":%lu,\"crc\":%lu,\"frame\":%lu,\"exception\":%lu,\"tags\":[",
             dev->unit_id, online ? "true" : "false", (unsigned long)st.cycles,
             (unsigned long)st.failed_cycles, (unsigned long)st.bus.ok,
             (unsigned long)st.bus.timeout, (unsigned long)st.bus.crc,
             (unsigned long)st.bus.frame, (unsigned long)st.bus.exception);
        for (uint16_t t = 0; t < dev->tag_count; t++) {
            const gw_tag_value_t *v = &s_vals[t];
            outf(&o, "%s{\"name\":", t ? "," : "");
            out_str(&o, c->tags[dev->first_tag + t].name);
            const gw_tag_cfg_t *tc = &c->tags[dev->first_tag + t];
            gw_val_kind_t kind = gw_conv_kind(tc);
            /* NaN is not JSON */
            bool valid = v->valid && (kind != GW_VK_DOUBLE || v->value == v->value);
            if (!valid) {
                outf(&o, ",\"valid\":false,\"value\":null}");
            } else if (kind == GW_VK_STRING) {
                outf(&o, ",\"valid\":true,\"value\":");
                out_str(&o, v->str);
                outf(&o, "}");
            } else if (kind == GW_VK_BYTES) {   /* SPB-19: hex */
                char hex[2 * GW_STR_MAX + 1];
                gw_conv_format(tc, v, hex, sizeof(hex));
                outf(&o, ",\"valid\":true,\"value\":\"%s\"}", hex);
            } else if (kind == GW_VK_INT64) {   /* exact (a JSON reader may round above 2^53) */
                outf(&o, ",\"valid\":true,\"value\":%lld}", (long long)v->i64);
            } else if (kind == GW_VK_UINT64) {
                outf(&o, ",\"valid\":true,\"value\":%llu}", (unsigned long long)v->u64);
            } else {
                /* %.9g: enough for float32 and integers up to 32 bits */
                outf(&o, ",\"valid\":true,\"value\":%.9g}", v->value);
            }
        }
        outf(&o, "]}");
    }
    gw_local_io_t io;
    gw_tagdb_get_io(&io);
    gw_io_expander_t exp;
    gw_io_get_expander(&exp);
    /* issue #74: unknown DOs (expander not answering) are null, never "off" */
    outf(&o, "],\"io\":{\"di\":%u,\"do\":", io.di);
    if (io.do_unknown) {
        outf(&o, "null");
    } else {
        outf(&o, "%u", io.dout);
    }
    outf(&o, ",\"do_unknown\":%s,\"expander\":{\"ready\":%s,\"retrying\":%s,\"attempts\":%u}",
         io.do_unknown ? "true" : "false", exp.ready ? "true" : "false",
         exp.retrying ? "true" : "false", exp.attempts);
    /* issue #75: what IO-3, IO-4, IO-6 and IO-7 added */
    outf(&o, ",\"failsafe\":%s,\"counters\":[", io.failsafe ? "true" : "false");
    for (int k = 0; k < GW_LOCAL_IO_CHANNELS; k++) {
        outf(&o, "%s%llu", k ? "," : "", (unsigned long long)io.count[k]);
    }
    uint8_t rule_do = 0;
    for (uint8_t r = 0; s_ctx.mode == GW_WEB_NORMAL && r < c->local_io.rule_count; r++) {
        rule_do |= (uint8_t)(1u << c->local_io.rules[r].dout);
    }
    outf(&o, "],\"rule_do\":%u}", rule_do);
    static const char *const alarm_keys[] = {"none", "device_offline", "mqtt_down", "config_error"};
    gw_alarm_t alarm = gw_buzzer_alarm();
    uint8_t enabled = gw_buzzer_alarms();
    outf(&o, ",\"buzzer\":{\"alarm\":\"%s\",\"enabled\":[", alarm_keys[alarm & 3]);
    bool first_alarm = true;
    for (int a = GW_ALARM_DEVICE_OFFLINE; a <= GW_ALARM_CONFIG_ERROR; a++) {
        if (enabled & GW_ALARM_BIT(a)) {
            outf(&o, "%s\"%s\"", first_alarm ? "" : ",", alarm_keys[a]);
            first_alarm = false;
        }
    }
    /* TIME-1..3 */
    gw_io_rtc_info_t rtc;
    gw_io_rtc_get(&rtc);
    gw_clock_source_t src = gw_edge_time_source();   /* #160: "valid", source "browser" */
    outf(&o, "]},\"time\":{\"utc_s\":%lld,\"synced\":%s,\"valid\":%s,\"source\":\"%s\","
             "\"rtc\":{\"present\":%s,\"status\":",
         (long long)time(NULL), src == GW_CLOCK_SNTP ? "true" : "false",
         gw_clock_valid(src) ? "true" : "false", gw_clock_source_key(src),
         rtc.present ? "true" : "false");
    out_str(&o, rtc.present ? gw_rtc_status_str((gw_rtc_status_t)rtc.status) : "not answering");
    outf(&o, ",\"stores\":%lu}}", (unsigned long)rtc.stores);
    status_card(&o, c);
    {   /* #132: is a core dump in flash (no flash access; the summary is a route of its own) */
        uint32_t size;
        gw_cdump_state_t cd = gw_coredump_state(&size);
        char buf[80];
        gw_cdump_status_json(cd, size, buf, sizeof(buf));
        outf(&o, ",\"core_dump\":%s", buf);
    }
    outf(&o,
         ",\"heap\":{\"internal_free\":%u,\"internal_min\":%u,"
         "\"psram_free\":%u}}\n",
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
         (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    out_flush(&o);
    return o.err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : o.err;
}

static esp_err_t favicon_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ---------- plain HTTP with HTTPS on: redirect only (issue #64) ---------- */

static esp_err_t redirect_any(httpd_req_t *req)
{
    char host[72] = "";
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK || host[0] == '\0') {
        snprintf(host, sizeof(host), "%s.local", s_ctx.cfg->network.hostname);
    }
    /* drop the port of the Host header ("[v6]:port" or "name:port") */
    char *colon = strrchr(host, ':');
    if (colon != NULL && (host[0] != '[' || colon > strchr(host, ']'))) {
        *colon = '\0';
    }
    /* redirect task only; "https://" + host + ":port" + the URI */
    static char loc[8 + sizeof(host) + 8 + CONFIG_HTTPD_MAX_URI_LEN + 1];
    char port[8] = "";
    if (s_ctx.cfg->web.https_port != 443) {
        snprintf(port, sizeof(port), ":%u", s_ctx.cfg->web.https_port);
    }
    snprintf(loc, sizeof(loc), "https://%s%s%s", host, port, req->uri);
    /* 301 for GET; 308 keeps the method and body of a PUT/POST/DELETE */
    httpd_resp_set_status(req, req->method == HTTP_GET ? "301 Moved Permanently"
                                                       : "308 Permanent Redirect");
    httpd_resp_set_hdr(req, "Location", loc);
    close_after_reply(req);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t start_redirect(uint16_t port)
{
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.server_port = port;
    hc.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 2;
    hc.task_priority = 3;
    hc.stack_size = 3072;
    hc.max_open_sockets = 2;
    hc.lru_purge_enable = true;
    hc.max_uri_handlers = 4;
    hc.uri_match_fn = httpd_uri_match_wildcard;
    ESP_RETURN_ON_ERROR(httpd_start(&s_redirect, &hc), TAG, "redirect server");
    guard_add(1, s_redirect, "HTTP redirect", port);
    static const handler_t redirect = {redirect_any};
    static const httpd_method_t methods[] = {HTTP_GET, HTTP_PUT, HTTP_POST, HTTP_DELETE};
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        const httpd_uri_t u = {.uri = "/*", .method = methods[i], .handler = guarded,
                               .user_ctx = (void *)&redirect};
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_redirect, &u), TAG, "redirect uri");
    }
    return ESP_OK;
}

bool gw_web_loop_alive(uint32_t max_age_ms)
{
    web_guard_t *g = &s_guard[0];
    if (g->server == NULL) {
        return false;
    }
    if (CONFIG_GW_WEB_STALL_S == 0) {
        return true;   /* no watchdog, no ping: the server runs */
    }
    return atomic_load(&g->in_handler) || now_ms32() - atomic_load(&g->alive_ms) < max_age_ms;
}

void gw_web_get_stats(gw_web_stats_t *out)
{
    out->running = s_server != NULL;
    out->https = s_https;
    out->failed = s_limit.failed_total;
    out->refused = s_limit.refused_total;
    out->blocks = s_limit.blocks_total;
    out->csrf_refused = s_csrf_refused;
    out->cut_bodies = atomic_load_explicit(&s_cut_bodies, memory_order_relaxed);
    out->stalls = atomic_load(&s_guard[0].stalls) + atomic_load(&s_guard[1].stalls);
    out->blocked_now = gw_authlimit_blocked(&s_limit, (uint64_t)(esp_timer_get_time() / 1000));
    out->password_set = atomic_load(&s_pw_set);
    uint32_t left_ms;
    out->claim_open = claim_window_open(&left_ms);
    out->claim_left_s = (left_ms + 999) / 1000;
    out->claim_refused = atomic_load(&s_claim_refused);
}

/* ---------- start ---------- */

esp_err_t gw_web_start(const gw_web_ctx_t *ctx)
{
    s_ctx = *ctx;
    if (ctx->mode == GW_WEB_NORMAL) {
        snprintf(s_user, sizeof(s_user), "%s", ctx->cfg->web.username);
        snprintf(s_hash, sizeof(s_hash), "%s", ctx->cfg->web.password_hash);
    } else {
        /* the user name lives in the rejected (or absent) document: any name is accepted */
        gw_secrets_t sec;
        gw_storage_get_secrets(&sec);
        s_user[0] = '\0';
        snprintf(s_hash, sizeof(s_hash), "%s", sec.web_password_hash);
        memset(&sec, 0, sizeof(sec));
    }
    atomic_store(&s_pw_set, s_hash[0] != '\0');
    const esp_timer_create_args_t ct = {.callback = claim_expired, .name = "claim_window"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&ct, &s_claim_timer), TAG, "claim timer");

    gw_authlimit_init(&s_limit, CONFIG_GW_WEB_LOGIN_MAX_FAILS, CONFIG_GW_WEB_LOGIN_BLOCK_S * 1000u,
                      CONFIG_GW_WEB_LOGIN_BLOCK_MAX_S * 1000u);
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.server_port = ctx->cfg->web.port;
    hc.task_priority = 3;
    hc.stack_size = 8192;            /* configuration validation runs in this task */
    hc.max_uri_handlers = WEB_URI_HANDLERS;
    hc.max_open_sockets = 4;
    hc.lru_purge_enable = true;
    const char *cert = NULL, *key = NULL;
    if (ctx->cfg->web.https && !gw_webcert_load(ctx->cfg->network.hostname, &cert, &key)) {
        ESP_LOGE(TAG, "HTTPS unavailable (no device certificate): plain HTTP on port %u",
                 ctx->cfg->web.port);
    }
    s_https = cert != NULL;
    if (s_https) {
        /* issue #64: HTTPS with the device certificate; a TLS session's mbedTLS buffers come
         * from the PSRAM (gw_storage_init, #135) */
        httpd_ssl_config_t sc = HTTPD_SSL_CONFIG_DEFAULT();
        sc.httpd = hc;
        sc.httpd.server_port = 0;
        sc.httpd.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1;
        sc.httpd.stack_size = 10240;   /* handshake + configuration validation */
        sc.httpd.max_open_sockets = 3;
        sc.port_secure = ctx->cfg->web.https_port;
        sc.servercert = (const uint8_t *)cert;
        sc.servercert_len = strlen(cert) + 1;
        sc.prvtkey_pem = (const uint8_t *)key;
        sc.prvtkey_len = strlen(key) + 1;
        ESP_RETURN_ON_ERROR(httpd_ssl_start(&s_server, &sc), TAG, "httpd_ssl_start");
        if (start_redirect(ctx->cfg->web.port) != ESP_OK) {
            ESP_LOGE(TAG, "no HTTP redirect on port %u", ctx->cfg->web.port);
        }
    } else {
        ESP_RETURN_ON_ERROR(httpd_start(&s_server, &hc), TAG, "httpd_start");
    }

    guard_add(0, s_server, s_https ? "HTTPS" : "HTTP",
              s_https ? ctx->cfg->web.https_port : ctx->cfg->web.port);
    static const handler_t h_index = {index_get}, h_favicon = {favicon_get},
                           h_status = {status_get}, h_config_get = {config_get},
                           h_config_put = {config_put}, h_restart = {restart_post},
                           h_fw_get = {firmware_get}, h_fw_post = {firmware_post},
                           h_fw_other = {firmware_other_post},
                           h_card_format = {card_format_post}, h_rebirth = {rebirth_post},
                           h_modbus_read = {modbus_read_post}, h_cd_get = {coredump_get},
                           h_cd_delete = {coredump_delete}, h_cd_summary = {coredump_summary_get},
                           h_time = {time_post}, h_claim_page = {claim_page_get},
                           h_claim_get = {claim_get}, h_claim_post = {claim_post};
    static const struct {
        const char *uri;
        httpd_method_t method;
        const handler_t *h;
    } uris[] = {
        {"/", HTTP_GET, &h_index},
        {"/favicon.ico", HTTP_GET, &h_favicon},
        {"/api/status", HTTP_GET, &h_status},
        {"/api/config", HTTP_GET, &h_config_get},
        {"/api/config", HTTP_PUT, &h_config_put},
        {"/api/config", HTTP_POST, &h_config_put},
        {"/api/restart", HTTP_POST, &h_restart},
        {"/api/firmware", HTTP_GET, &h_fw_get},     /* FW-1 */
        {"/api/firmware", HTTP_POST, &h_fw_post},
        {"/api/firmware/other", HTTP_POST, &h_fw_other},   /* #144 */
        {"/api/card/format", HTTP_POST, &h_card_format},   /* #139 */
        {"/api/rebirth", HTTP_POST, &h_rebirth},           /* #171 */
        {"/api/modbus/read", HTTP_POST, &h_modbus_read},   /* #182 */
        {"/api/coredump", HTTP_GET, &h_cd_get},            /* #132 */
        {"/api/coredump", HTTP_DELETE, &h_cd_delete},
        {"/api/coredump/summary", HTTP_GET, &h_cd_summary},
        {"/api/time", HTTP_POST, &h_time},                 /* #160 */
        {"/claim", HTTP_GET, &h_claim_page},               /* #120 */
        {"/api/claim", HTTP_GET, &h_claim_get},
        {"/api/claim", HTTP_POST, &h_claim_post},
    };
    _Static_assert(sizeof(uris) / sizeof(uris[0]) == WEB_URI_HANDLERS, "WEB_URI_HANDLERS");
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        const httpd_uri_t u = {.uri = uris[i].uri, .method = uris[i].method, .handler = guarded,
                               .user_ctx = (void *)uris[i].h};
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &u), TAG, "%s", uris[i].uri);
    }
    guard_start();
    if (s_https) {
        ESP_LOGI(TAG, "HTTPS on port %u; HTTP on port %u only redirects to it",
                 ctx->cfg->web.https_port, ctx->cfg->web.port);
    }
    ESP_LOGI(TAG, "web UI and REST API on port %u (%s)",
             s_https ? ctx->cfg->web.https_port : hc.server_port,
             s_hash[0] == '\0' ? "user 'admin' with the commissioning password: no web password set"
             : s_user[0] != '\0' ? "web.username and web.password"
                                : "stored web password, any user name");
    return ESP_OK;
}

void gw_web_stack_free(unsigned *web, unsigned *redirect)
{
    *web = s_guard[0].task != NULL ? (unsigned)uxTaskGetStackHighWaterMark(s_guard[0].task) : 0;
    *redirect = s_guard[1].task != NULL ? (unsigned)uxTaskGetStackHighWaterMark(s_guard[1].task) : 0;
}
