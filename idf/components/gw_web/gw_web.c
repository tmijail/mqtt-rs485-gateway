#include "gw_web.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
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
#include "gw_ap.h"
#include "gw_auth.h"
#include "gw_ident.h"
#include "gw_authlimit.h"
#include "gw_bus.h"
#include "gw_fw.h"
#include "gw_buzz.h"
#include "gw_buzzer.h"
#include "gw_cdump.h"
#include "gw_certs.h"
#include "gw_claim.h"
#include "gw_conv.h"
#include "gw_coredump.h"
#include "gw_edge.h"
#include "gw_io.h"
#include "gw_iocmd.h"
#include "gw_rtc.h"
#include "gw_sfcard.h"
#include "gw_log.h"
#include "gw_mbdiag.h"
#include "gw_net.h"
#include "gw_ota.h"
#include "gw_pemfile.h"
#include "gw_status.h"
#include "gw_storage.h"
#include "gw_tagdb.h"
#include "gw_temp.h"
#include "gw_webcert.h"
#include "lwip/sockets.h"

static const char *TAG = "web";

#define REALM_HDR "Basic realm=\"gateway\", charset=\"UTF-8\""
#define WEB_URI_HANDLERS 35   /* the routes of gw_web_start (uris[]) */

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
 * does not prevent one (docs/reference/commissioning-and-secrets.md#the-limits-of-the-web-server).
 * The ping also records each server's task, so the shell can tell the two "httpd" tasks apart.
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

/* The writer of the answers of the server task (/api/status, the claim): one buffer in .bss,
 * shared because the server task serves one request at a time. */
static out_t s_out;

static out_t *out_begin(httpd_req_t *req)
{
    s_out.req = req;
    s_out.len = 0;
    s_out.err = ESP_OK;
    return &s_out;
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

void gw_web_claim_close(const char *why)
{
    if (s_claim_timer == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_claim_mux);
    bool was = gw_claim_active(&s_claim, now_ms64());
    gw_claim_close(&s_claim);
    portEXIT_CRITICAL(&s_claim_mux);
    if (was) {
        esp_timer_stop(s_claim_timer);
        ESP_LOGW(TAG, "claim window closed: %s", why);
        if (s_ctx.claim_closed != NULL) {
            s_ctx.claim_closed(false);
        }
    }
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

/* The length of a file of EMBED_TXTFILES, without the NUL that ESP-IDF appends. The start and
 * the end are two objects for C: a subtraction of the two pointers is undefined behaviour, a
 * subtraction of the two addresses is not (-Wanalyzer-undefined-behavior-ptrdiff, #230). */
static ssize_t embedded_len(const char *start, const char *end)
{
    return (ssize_t)((uintptr_t)end - (uintptr_t)start) - 1;
}

extern const char claim_html_start[] asm("_binary_claim_html_start");
extern const char claim_html_end[] asm("_binary_claim_html_end");

/* GET /claim: the page of the window. It holds nothing secret: no credential. */
static esp_err_t claim_page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, claim_html_start, embedded_len(claim_html_start, claim_html_end));
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

/* Issue #187: the answer of a successful claim, for the success page: the user name; the access
 * point (its SSID, the address of this connection, the seconds to the change to WPA2 with the
 * new password); the addresses of the gateway on the plant network and its mDNS name (NET-6). */
static __attribute__((noinline)) esp_err_t claim_answer(httpd_req_t *req)
{
    const gw_config_t *c = s_ctx.cfg;
    const char *scheme = s_https ? "https" : "http";
    unsigned port = s_https ? c->web.https_port : c->web.port;
    char sfx[8] = "", url[96] = "", ssid[33];
    if (port != (s_https ? 443u : 80u)) {
        snprintf(sfx, sizeof(sfx), ":%u", port);
    }
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    out_t *o = out_begin(req);
    outf(o, "{\"ok\":true,\"username\":");
    out_str(o, s_user);
    gw_ap_ssid(ssid);
    outf(o, ",\"ap\":{\"ssid\":");
    out_str(o, ssid);
    struct sockaddr_storage la;
    socklen_t ln = sizeof(la);
    uint8_t local[4];
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&la, &ln) == 0 &&
        ipv4_of(&la, local)) {
        snprintf(url, sizeof(url), "%s://%u.%u.%u.%u%s/", scheme, local[0], local[1], local[2],
                 local[3], sfx);
    }
    outf(o, ",\"url\":\"%s\",\"secure_in_s\":%u},\"plant\":[", url,
         (unsigned)(GW_APLIFE_CLAIM_SECURE_MS / 1000u));
    gw_net_if_status_t ifs[GW_NET_IF_COUNT];
    gw_net_get_status(ifs);
    bool first = true;
    for (int i = 0; i < GW_NET_IF_COUNT; i++) {
        const gw_net_if_status_t *n = &ifs[i];
        if (n->ipv4[0] | n->ipv4[1] | n->ipv4[2] | n->ipv4[3]) {
            outf(o, "%s\"%s://%u.%u.%u.%u%s/\"", first ? "" : ",", scheme, n->ipv4[0], n->ipv4[1],
                 n->ipv4[2], n->ipv4[3], sfx);
            first = false;
        }
        for (int k = 0; k < GW_NET_IPV6_MAX; k++) {
            /* a link-local address needs a zone in the browser: left out */
            if (n->ipv6[k][0] != '\0' && strncasecmp(n->ipv6[k], "fe80", 4) != 0) {
                outf(o, "%s\"%s://[%s]%s/\"", first ? "" : ",", scheme, n->ipv6[k], sfx);
                first = false;
            }
        }
    }
    outf(o, "],\"mdns\":");
#if CONFIG_GW_MDNS
    if (c->network.mdns) {
        snprintf(url, sizeof(url), "%s://%s.local%s/", scheme, c->network.hostname, sfx);
        out_str(o, url);
    } else {
        outf(o, "null");
    }
#else
    outf(o, "null");
#endif
    outf(o, "}\n");
    out_flush(o);
    return o->err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : o->err;
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
    esp_err_t ret = claim_answer(req);
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
     * NVS migration copy (docs/reference/commissioning-and-secrets.md#the-nvs-migration-in-the-qemu-build):
     * formats LittleFS, the copy is lost */
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
    if (query_flag(req, "safe")) {
        /* issue #209: safe mode (no polling, no MQTT) for 60 min or until the next restart */
        char why[160];
        if (!gw_fw_request_safe_mode(why, sizeof(why))) {
            return send_error(req, "409 Conflict", NULL, why);
        }
        ESP_LOGW(TAG, "restart into safe mode requested through the REST API");
        send_json(req, "202 Accepted", "{\"ok\":true,\"restart\":true,\"safe_mode\":true}\n");
        s_ctx.restart();
        return ESP_OK;
    }
    ESP_LOGW(TAG, "restart requested through the REST API");
    send_json(req, "202 Accepted", "{\"ok\":true,\"restart\":true}\n");
    s_ctx.restart();
    return ESP_OK;
}

/* ---------- #203: POST /api/factory-reset?confirm=1 ---------- */

/* The same checks as every write (CSRF, then the password); the request must say ?confirm=1 (the
 * web UI asks the operator to type ERASE first). The erase is the one of BOOT held 10 s: the
 * answer comes after it, and the gateway restarts in commissioning 0.5 s later. */
static esp_err_t factory_reset_post(httpd_req_t *req)
{
    if (!same_origin(req) || !authorized(req)) {
        return ESP_OK;
    }
    if (!query_flag(req, "confirm")) {
        return send_error(req, "400 Bad Request", NULL,
                          "missing ?confirm=1: the factory reset erases the configuration, the "
                          "secrets, the web certificate, the web password and the counters");
    }
    ESP_LOGW(TAG, "factory reset requested through the REST API");
    esp_err_t e = s_ctx.factory_reset();
    if (e == ESP_ERR_INVALID_STATE) {
        return send_error(req, "409 Conflict", NULL, "factory reset refused: nothing erased (see "
                                                     "the log)");
    }
    char body[120];
    snprintf(body, sizeof(body), "{\"ok\":true,\"erased\":%s,\"restart\":\"commissioning\"}\n",
             e == ESP_OK ? "true" : "false");
    return send_json(req, "202 Accepted", body);
}

/* ---------- #207: POST /api/identify ---------- */

/* The LED and the buzzer identify the gateway for 10 s. Harmless, but a write all the same (CSRF,
 * then the password). 409 while one runs: its time does not start again. */
static esp_err_t identify_post(httpd_req_t *req)
{
    if (!same_origin(req) || !authorized(req)) {
        return ESP_OK;
    }
    char body[120];
    if (!gw_status_identify()) {
        unsigned long left = (unsigned long)gw_status_identify_left_s();
        snprintf(body, sizeof(body),
                 "{\"ok\":false,\"error\":\"identify already runs: %lu s left\","
                 "\"identify_left_s\":%lu}\n", left, left);
        return send_json(req, "409 Conflict", body);
    }
    ESP_LOGI(TAG, "identify requested through the REST API");
    snprintf(body, sizeof(body), "{\"ok\":true,\"identify_left_s\":%u}\n", GW_IDENT_MS / 1000);
    return send_json(req, "202 Accepted", body);
}

/* ---------- #200: GET /api/log?since=<n> ---------- */

/* One log line as a JSON string: a line can be up to GW_LOGRING_LINE_MAX bytes, and each '"' or
 * '\\' takes two (the ring has no control characters). */
static void out_line(out_t *o, const char *p, size_t n)
{
    char raw[GW_LOGRING_LINE_MAX + 1], e[2 * GW_LOGRING_LINE_MAX + 8];
    n = n < GW_LOGRING_LINE_MAX ? n : GW_LOGRING_LINE_MAX;
    memcpy(raw, p, n);
    raw[n] = '\0';
    json_escape(e, sizeof(e), raw);
    outf(o, "\"%s\"", e);
}

/* The lines after number "since" (0: every line in the ring). The web UI asks only while its card
 * Log is open (the decision of #200). "lost": the lines after "since" that the ring no longer
 * has; "next" lower than "since": the gateway restarted (the numbers start again at 1). */
static esp_err_t log_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    char q[48], v[16];
    uint32_t since = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK) {
        since = (uint32_t)strtoul(v, NULL, 10);
    }
    const char *text;
    size_t len;
    gw_log_ring_info_t in;
    if (!gw_log_ring_copy(since, &text, &len, &in)) {
        return send_error(req, "503 Service Unavailable", NULL,
                          "no live log: no memory for its buffer (or it is busy)");
    }
    if (since >= in.next && since != 0) {   /* a restart: every line again */
        since = 0;
        gw_log_ring_copy(0, &text, &len, &in);
    }
    uint32_t got = len > 0 ? in.from : in.next;   /* the first line after the lost ones */
    uint32_t lost = since != 0 && got > since + 1 ? got - since - 1 : 0;
    out_t *o = out_begin(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    outf(o, "{\"since\":%lu,\"last\":%lu,\"next\":%lu,\"first\":%lu,\"lost\":%lu,"
            "\"count\":%lu,\"bytes\":%u,\"size\":%u,\"dropped\":%lu,\"lines\":[",
         (unsigned long)since, (unsigned long)(len > 0 ? in.last : in.next - 1),
         (unsigned long)in.next, (unsigned long)in.first, (unsigned long)lost,
         (unsigned long)in.lines, (unsigned)in.used, (unsigned)in.size,
         (unsigned long)in.dropped);
    for (size_t at = 0, k = 0; at < len; k++) {
        const char *nl = memchr(text + at, '\n', len - at);
        size_t n = nl != NULL ? (size_t)(nl - (text + at)) : len - at;
        if (k > 0) {
            outf(o, ",");
        }
        out_line(o, text + at, n);
        at += n + 1;
    }
    outf(o, "]}\n");
    out_flush(o);
    return o->err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : o->err;
}

/* ---------- #199: GET and POST /api/log/level ---------- */

/* The levels that the build offers, the tags for the selector of the web UI (the tags of the
 * firmware and the tags that the log showed; the web UI asks when its card Log opens) and the
 * changed levels. */
static esp_err_t log_level_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    const size_t cap = GW_LOGTAG_MAX * (GW_LOGTAG_SIZE + 32) + 8;
    char *tags = gw_storage_malloc(2 * cap);   /* PSRAM, for this request only */
    if (tags == NULL) {
        return send_error(req, "503 Service Unavailable", NULL, "no memory");
    }
    char *changed = tags + cap;
    gw_log_tags_json(false, tags, cap);
    gw_log_tags_json(true, changed, cap);
    out_t *o = out_begin(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    outf(o, "{\"build\":\"%s\",\"max\":\"%s\",\"levels\":[",
         gw_loglevel_name(gw_log_build_level()), gw_loglevel_name(gw_log_max_level()));
    for (int l = GW_LOGLV_NONE; l <= gw_log_max_level() && l <= GW_LOGLV_VERBOSE; l++) {
        outf(o, "%s\"%s\"", l ? "," : "", gw_loglevel_name(l));
    }
    outf(o, "],\"tags\":");
    out_flush(o);
    if (o->err == ESP_OK) {
        o->err = httpd_resp_send_chunk(req, tags, (ssize_t)strlen(tags));
    }
    outf(o, ",\"changed\":%s}\n", changed);
    out_flush(o);
    heap_caps_free(tags);
    return o->err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : o->err;
}

/* {"tag": "poll", "level": "debug"} or {"reset": true}: esp_log_level_set, not saved (a restart
 * sets the level of the build again). Same checks as every write (CSRF, then the password). */
static esp_err_t log_level_post(httpd_req_t *req)
{
    if (!same_origin(req) || !authorized(req)) {
        return ESP_OK;
    }
    size_t len;
    const char *status;
    char *body = req->content_len <= 128 ? read_body(req, &len, &status) : NULL;
    if (body == NULL) {
        return refuse_body(req, "400 Bad Request",
                           "expected {\"tag\": \"<tag or *>\", \"level\": \"<level>\"} or "
                           "{\"reset\": true} (at most 128 bytes)");
    }
    char tag[GW_LOGTAG_SIZE], why[120];
    int level = 0;
    bool reset = false;
    bool ok = gw_logring_parse_set(body, len, gw_log_max_level(), tag, &level, &reset, why,
                                   sizeof(why));
    heap_caps_free(body);
    if (!ok) {
        return send_error(req, "400 Bad Request", NULL, why);
    }
    if (reset) {
        gw_log_level_reset();
        return send_json(req, "200 OK", "{\"ok\":true,\"reset\":true}\n");
    }
    if (gw_log_level_set(tag, level) != GW_LOG_SET_OK) {
        return send_error(req, "409 Conflict", NULL, "no room for one more tag");
    }
    char answer[96];
    snprintf(answer, sizeof(answer), "{\"ok\":true,\"tag\":\"%s\",\"level\":\"%s\"}\n", tag,
             gw_loglevel_name(level));
    return send_json(req, "200 OK", answer);
}

/* ---------- NET-5, issue #187: POST /api/ap/stop ---------- */

/* The button "Close the access point". The same checks as every write (CSRF, then the
 * password). The access point stops a short time after the answer, so that a client of the
 * access point gets it. A BOOT gesture of 2-10 s starts it again. */
static esp_err_t ap_stop_post(httpd_req_t *req)
{
    if (!same_origin(req) || !authorized(req)) {
        return ESP_OK;
    }
    if (!gw_ap_stop_soon()) {
        return send_error(req, "409 Conflict", NULL, "the access point does not run");
    }
    ESP_LOGW(TAG, "access point stop requested through the REST API");
    return send_json(req, "202 Accepted", "{\"ok\":true,\"stopping\":true}\n");
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

/* ---------- #204: POST /api/do ---------- */

/* {"do": 1-8, "on": true|false}: switch a DO from the web UI. The command goes to the local_io
 * task through the queue of the NCMD commands (gw_io_command_do), with the same checks: a DO
 * that a local rule drives is refused (409), and the new state goes to the Sparkplug host in the
 * next NDATA (tck-id-topics-ndata-payload), as for an NCMD. Only the DOs: the web UI does not
 * write to the Modbus devices. Same checks as every write (CSRF, then the password). */
static esp_err_t do_post(httpd_req_t *req)
{
    if (!same_origin(req) || !authorized(req)) {
        return ESP_OK;
    }
    if (s_ctx.mode != GW_WEB_NORMAL) {
        return refuse_body(req, "409 Conflict",
                           s_ctx.mode == GW_WEB_FAULT
                               ? "the local I/O does not run: the gateway does not start "
                                 "(hardware fault, see the status)"
                               : "the local I/O does not run: the gateway has no valid "
                                 "configuration. Correct it and save it first");
    }
    size_t len;
    const char *status;
    char *body = req->content_len <= 128 ? read_body(req, &len, &status) : NULL;
    if (body == NULL) {
        return refuse_body(req, "400 Bad Request",
                           "expected {\"do\": 1-8, \"on\": true|false} (at most 128 bytes)");
    }
    uint8_t index;
    bool on;
    char why[96];
    bool parsed = gw_iocmd_parse(body, len, &index, &on, why, sizeof(why));
    heap_caps_free(body);
    if (!parsed) {
        return send_error(req, "400 Bad Request", NULL, why);
    }
    gw_io_do_result_t r = gw_io_command_do(GW_IO_CALLER_WEB, index, on);
    if (r == GW_IO_DO_RULE) {
        snprintf(why, sizeof(why), "DO/%u is driven by a local rule (local_io.rules): refused",
                 index + 1u);
        return send_error(req, "409 Conflict", NULL, why);
    }
    if (r != GW_IO_DO_OK) {
        return send_error(req, "503 Service Unavailable", NULL,
                          r == GW_IO_DO_BUSY ? "the local I/O task did not answer: try again"
                                             : "the output expander does not answer: the DOs "
                                               "are unknown");
    }
    gw_local_io_t io;
    gw_tagdb_get_io(&io);
    char answer[96];
    snprintf(answer, sizeof(answer), "{\"ok\":true,\"do\":%u,\"on\":%s,\"state\":%s}\n",
             index + 1u, on ? "true" : "false", (io.dout >> index) & 1u ? "true" : "false");
    return send_json(req, "200 OK", answer);
}

/* ---------- #160: POST /api/time ---------- */

/* TIME-1 / issue #160: the web UI's button "Set the time from the browser", for a network with
 * no time server and a gateway with no RTC time. {"utc_ms": <ms UTC>}: the gateway sets its
 * clock and the RTC; in normal mode the Sparkplug task then connects (no MQTT session before a
 * valid clock: the decision of 2026-11-14). Same checks as every write (CSRF, then the password); in every mode
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
        char sha[2 * GW_OTA_SHA256_HEX + 48];
        char body[2200];
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
    /* issue #215: the two SHA-256 values when they did not agree */
    j->sha[0] = '\0';
    if (j->f.sha_mismatch) {
        char want[GW_OTA_SHA256_HEX], file[GW_OTA_SHA256_HEX];
        gw_ota_sha256_hex(j->f.sha_expected, want);
        gw_ota_sha256_hex(j->f.sha_file, file);
        snprintf(j->sha, sizeof(j->sha), ",\"sha256_expected\":\"%s\",\"sha256_file\":\"%s\"",
                 want, file);
    }
    /* issue #144: both slots, the trial (expected mode, time left), the boot-loop counter. #223:
     * the button and the web upload are always possible ("mqtt": the value of ota.mqtt) */
    bool can_boot = !j->f.trial && j->f.phase != GW_FW_DOWNLOADING &&
                    j->f.phase != GW_FW_RESTARTING && j->f.other_state != GW_FW_SLOT_EMPTY &&
                    j->f.other_state != GW_FW_SLOT_INVALID;
    snprintf(j->body, sizeof(j->body),
             "{\"running\":\"%s\",\"slot\":\"%s\",\"other\":\"%s\",\"other_slot\":\"%s\","
             "\"other_state\":\"%s\",\"other_why\":\"%s\",\"can_boot_other\":%s,"
             "\"mqtt\":%s,\"phase\":\"%s\",\"state\":\"%s\",\"detail\":\"%s\"%s,"
             "\"progress\":%u,\"bytes\":%lu,\"total\":%lu,\"trial\":%s,"
             "\"trial_left_s\":%lu,\"expected_mode\":\"%s\",\"boot_count\":%lu,"
             "\"safe_mode\":%s,\"cfg_prev\":{\"version\":\"%s\",\"saved\":%lld,"
             "\"schema\":%u},\"cfg_prev_restore\":%s}\n",
             j->run, j->f.slot, j->other, j->f.other_slot,
             j->f.other_state < sizeof(k_fw_slot) / sizeof(k_fw_slot[0])
                 ? k_fw_slot[j->f.other_state] : "?",
             j->other_why, can_boot ? "true" : "false", j->f.mqtt ? "true" : "false",
             j->f.phase < sizeof(k_fw_phase) / sizeof(k_fw_phase[0]) ? k_fw_phase[j->f.phase] : "?",
             j->text, j->detail, j->sha, j->f.progress, (unsigned long)j->f.bytes,
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

/* Issue #215: the SHA-256 of an uploaded file does not agree: 422 with the two values. The
 * buffers are in PSRAM, not on the stack of the server task (it is not inlined into the upload). */
static esp_err_t __attribute__((noinline)) send_sha_mismatch(httpd_req_t *req, const char *why)
{
    typedef struct {
        gw_fw_status_t f;
        char want[GW_OTA_SHA256_HEX];
        char file[GW_OTA_SHA256_HEX];
        char msg[2 * 160];
        char body[2 * 160 + 2 * GW_OTA_SHA256_HEX + 128];
    } sha_json_t;
    sha_json_t *j = gw_storage_malloc(sizeof(*j));
    if (j == NULL) {
        return send_error(req, "422 Unprocessable Content", NULL, why);
    }
    gw_fw_get_status(&j->f);
    j->want[0] = j->file[0] = '\0';
    if (j->f.sha_mismatch) {
        gw_ota_sha256_hex(j->f.sha_expected, j->want);
        gw_ota_sha256_hex(j->f.sha_file, j->file);
    }
    json_escape(j->msg, sizeof(j->msg), why);
    snprintf(j->body, sizeof(j->body),
             "{\"ok\":false,\"error\":\"%s\",\"path\":\"\",\"line\":0,\"column\":0,"
             "\"sha256_expected\":\"%s\",\"sha256_file\":\"%s\"}\n", j->msg, j->want, j->file);
    esp_err_t r = send_json(req, "422 Unprocessable Content", j->body);
    heap_caps_free(j);
    return r;
}

/* {"url": "https://..."} (application/json): the gateway downloads it.
 * application/octet-stream: the body is the image itself, written as it arrives. Any other
 * content type is refused (issue #100: text/plain is what a foreign page can send).
 * Issue #144 item 3: "force": true in the JSON, or ?force=1 with an image, installs an image
 * that the pre-flight check refuses (409 without it); it must then reach recovery mode.
 * Issue #215: "sha256" in the JSON, or the header X-Image-SHA256 with an image: the SHA-256 that
 * the file must have (64 hexadecimal characters; empty = no check). A file with another SHA-256
 * is not installed: the upload answers 422 with the two values (GET gives them too). */
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
        bool force = false, has_sha = false;
        uint8_t sha[GW_OTA_SHA256_LEN];
        bool parsed = gw_ota_parse_request(body, len, url, &force, sha, &has_sha, why,
                                           sizeof(why));
        heap_caps_free(body);
        if (!parsed) {
            return send_error(req, "400 Bad Request", NULL, why);
        }
        esp_err_t e = gw_fw_start_url(url, has_sha ? sha : NULL, force, why, sizeof(why));
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
    /* issue #215: the optional SHA-256 of the image */
    char hex[80] = "";
    uint8_t sha[GW_OTA_SHA256_LEN];
    bool has_sha = httpd_req_get_hdr_value_str(req, "X-Image-SHA256", hex, sizeof(hex)) == ESP_OK &&
                   hex[0] != '\0';
    if (has_sha) {
        char reason[96];
        if (!gw_ota_sha256_parse(hex, sha, reason, sizeof(reason))) {
            snprintf(why, sizeof(why), "X-Image-SHA256: %s", reason);
            return refuse_body(req, "400 Bad Request", why);
        }
    } else if (httpd_req_get_hdr_value_len(req, "X-Image-SHA256") >= sizeof(hex)) {
        return refuse_body(req, "400 Bad Request",
                           "X-Image-SHA256: expected 64 hexadecimal characters (0-9, a-f)");
    }
    esp_err_t e = gw_fw_upload_begin(req->content_len, has_sha ? sha : NULL,
                                     query_flag(req, "force"), why, sizeof(why));
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
    if (e == ESP_ERR_INVALID_CRC) {
        return send_sha_mismatch(req, why);   /* issue #215 */
    }
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

/* Issue #256: the summary maps the coredump partition; in the QEMU build it runs on core 0 with
 * core 1 parked (gw_fw_run_mapped) */
typedef struct {
    gw_cdump_info_t *info;
    esp_err_t err;
} cd_summary_call_t;

static void cd_summary(void *arg)
{
    cd_summary_call_t *c = arg;
    c->err = gw_coredump_summary(c->info);
}

/* Only the summary of a valid dump maps the partition. The state becomes valid only at boot, so
 * a dump that is not valid now stays so. */
static esp_err_t coredump_summary(gw_cdump_info_t *out)
{
    cd_summary_call_t c = {.info = out, .err = ESP_FAIL};
    uint32_t size;
    if (gw_coredump_state(&size) == GW_CDUMP_VALID) {
        gw_fw_run_mapped(cd_summary, &c);
    } else {
        cd_summary(&c);
    }
    return c.err;
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
    if (coredump_summary(&j->i) != ESP_OK ||
        gw_cdump_summary_json(&j->i, s_ctx.info->reset_reason, j->body, sizeof(j->body)) == 0) {
        r = send_error(req, "503 Service Unavailable", NULL, "no memory for the summary");
    } else {
        r = send_json(req, "200 OK", j->body);
    }
    heap_caps_free(j);
    return r;
}

/* ---------- #208 #221: a JSON answer in a PSRAM buffer ---------- */

typedef struct {
    char *b;
    size_t cap, n;
} jbuf_t;

static void jb_add(jbuf_t *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void jb_add(jbuf_t *j, const char *fmt, ...)
{
    if (j->n >= j->cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(j->b + j->n, j->cap - j->n, fmt, ap);
    va_end(ap);
    j->n = r < 0 ? j->cap : j->n + (size_t)r;
}

/* "<key>":"<escaped text>" */
static void jb_str(jbuf_t *j, const char *key, const char *s)
{
    char esc[2 * 320 + 8];   /* the SANs of a certificate (gw_webcert_info_t) */
    json_escape(esc, sizeof(esc), s);
    jb_add(j, "\"%s\":\"%s\"", key, esc);
}

static esp_err_t jb_send(httpd_req_t *req, const char *status, jbuf_t *j)
{
    if (j->n >= j->cap) {
        return send_error(req, "500 Internal Server Error", NULL, "answer too long");
    }
    esp_err_t r = send_json(req, status, j->b);
    return r;
}

/* ---------- #208: GET /api/wifi/scan ---------- */

#define WIFI_SCAN_MAX_AGE_S 10   /* a new scan only when the last result is older */

static void scan_one(void *ctx, const gw_net_scan_ap_t *ap)
{
    jbuf_t *j = ctx;
    char ssid[2 * sizeof(ap->ssid) + 8];
    json_escape(ssid, sizeof(ssid), ap->ssid);
    jb_add(j, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%u,\"security\":\"%s\",\"supported\":%s}",
           j->b[j->n - 1] == '[' ? "" : ",", ssid, ap->rssi, ap->channel, ap->security,
           ap->supported ? "true" : "false");
}

/* The Wi-Fi networks near the gateway, for the button "Find networks" of the web UI: ?new=1
 * starts a scan (unless one runs, or the last result is younger than WIFI_SCAN_MAX_AGE_S); a
 * request without it only reads the state. 202 while the scan runs, 200 with the networks. The
 * scan also works with network.wifi off. QEMU has no radio: the list is simulated. */
static esp_err_t wifi_scan_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    if (query_flag(req, "new")) {
        gw_net_wifi_scan_start(WIFI_SCAN_MAX_AGE_S);
    }
    uint32_t age;
    bool sim;
    char why[64];
    gw_net_scan_state_t st = gw_net_wifi_scan_result(NULL, NULL, &age, &sim, why, sizeof(why));
    switch (st) {
    case GW_NET_SCAN_UNAVAILABLE:
        return send_error(req, "404 Not Found", NULL, "this build has no Wi-Fi radio");
    case GW_NET_SCAN_IDLE:
        return send_json(req, "200 OK", "{\"state\":\"idle\",\"networks\":[]}\n");
    case GW_NET_SCAN_RUNNING:
        return send_json(req, "202 Accepted", "{\"state\":\"scanning\"}\n");
    case GW_NET_SCAN_FAILED: {
        char body[160], esc[128];
        json_escape(esc, sizeof(esc), why);
        snprintf(body, sizeof(body), "{\"state\":\"failed\",\"error\":\"%s\"}\n", esc);
        return send_json(req, "503 Service Unavailable", body);
    }
    default:
        break;
    }
    jbuf_t j = {.cap = 192 + GW_NET_SCAN_MAX * 160};
    j.b = gw_storage_malloc(j.cap);
    if (j.b == NULL) {
        return send_error(req, "503 Service Unavailable", NULL, "no memory");
    }
    jb_add(&j, "{\"state\":\"done\",\"age_s\":%u,\"simulated\":%s,\"networks\":[", (unsigned)age,
           sim ? "true" : "false");
    gw_net_wifi_scan_result(scan_one, &j, &age, &sim, why, sizeof(why));
    jb_add(&j, "]}\n");
    esp_err_t r = jb_send(req, "200 OK", &j);
    gw_storage_free(j.b);
    return r;
}

/* ---------- #221: the PEM files next to config.json ---------- */

#define FILES_MAX GW_PEM_FILES_MAX   /* files that GET /api/files lists */

typedef struct {
    char (*names)[GW_FILE_SIZE];
    size_t count;
} names_t;

static void file_name(void *ctx, const char *name, size_t size)
{
    names_t *n = ctx;
    (void)size;
    if (n->count < FILES_MAX) {
        snprintf(n->names[n->count++], GW_FILE_SIZE, "%s", name);
    }
}

/* The value of @p key in the query (a longer buffer than query_flag: a load names two files). */
static bool query_get(httpd_req_t *req, const char *key, char *out, size_t cap)
{
    char q[192];
    out[0] = '\0';
    return httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
           httpd_query_key_value(q, key, out, cap) == ESP_OK;
}

static bool query_one(httpd_req_t *req, const char *key)
{
    char v[8];
    return query_get(req, key, v, sizeof(v)) && strcmp(v, "1") == 0;
}

/* "used_by":["mqtt.tls.ca_file",...]: the keys of the saved configuration that name the file */
static void jb_used(jbuf_t *j, unsigned mask)
{
    jb_add(j, ",\"used_by\":[");
    unsigned m = 0;
    for (unsigned r = 0; r < GW_FILEREF_COUNT; r++) {
        if (mask & (1u << r)) {
            jb_add(j, "%s\"%s\"", m++ ? "," : "", gw_fileref_path((gw_fileref_t)r));
        }
    }
    jb_add(j, "]");
}

static void jb_info(jbuf_t *j, const gw_pem_info_t *f)
{
    jb_add(j, "{");
    jb_str(j, "name", f->name);
    jb_add(j, ",\"size\":%u,\"kind\":\"%s\"", (unsigned)f->size,
           f->kind == GW_PEM_CERTS ? "certificates" : f->kind == GW_PEM_KEY ? "key" : "other");
    if (f->kind == GW_PEM_CERTS) {
        jb_add(j, ",\"count\":%u,\"certs\":[", f->count);
        for (unsigned i = 0; i < f->count && i < GW_PEM_CERTS_MAX; i++) {
            jb_add(j, "%s{", i ? "," : "");
            jb_str(j, "subject", f->certs[i].subject);
            jb_add(j, ",\"not_after\":\"%s\",\"ca\":%s}", f->certs[i].not_after,
                   f->certs[i].ca ? "true" : "false");
        }
        jb_add(j, "]");
    } else if (f->kind == GW_PEM_KEY) {
        jb_add(j, ",");
        jb_str(j, "key_type", f->key_type);
    }
}

/* GET /api/files: the files that the operator can name in mqtt.tls and ota (the selects of the
 * card MQTT, the list of the card Certificates), with the subject and the expiry of each
 * certificate, the type of each private key, the certificate files that it agrees with and the
 * keys of the saved configuration that use each file. Never the content of a file. */
static esp_err_t files_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    names_t n = {.names = gw_storage_calloc(FILES_MAX, GW_FILE_SIZE)};
    gw_pem_info_t *info = gw_storage_calloc(FILES_MAX, sizeof(*info));
    jbuf_t j = {.cap = 384 + FILES_MAX * (300 + GW_PEM_CERTS_MAX * (2 * GW_PEM_SUBJECT_MAX + 64))};
    j.b = gw_storage_malloc(j.cap);
    esp_err_t r;
    if (n.names == NULL || info == NULL || j.b == NULL) {
        r = send_error(req, "503 Service Unavailable", NULL, "no memory");
    } else {
        size_t all = gw_storage_list_files(file_name, &n);
        gw_filerefs_t refs;
        bool known = gw_storage_file_refs(&refs);
        size_t total = 0;
        for (size_t i = 0; i < n.count; i++) {
            char why[96];
            if (!gw_pem_info(n.names[i], &info[i], why, sizeof(why))) {
                info[i].kind = GW_PEM_OTHER;
            }
            total += info[i].size;
        }
        jb_add(&j, "{\"files\":[");
        for (size_t i = 0; i < n.count; i++) {
            jb_add(&j, "%s", i ? "," : "");
            jb_info(&j, &info[i]);
            if (info[i].kind == GW_PEM_KEY) {
                jb_add(&j, ",\"agrees_with\":[");
                unsigned m = 0;
                for (size_t k = 0; k < n.count; k++) {
                    if (info[k].kind == GW_PEM_CERTS && gw_pem_key_agrees(info[i].name, info[k].name)) {
                        jb_add(&j, "%s\"%s\"", m++ ? "," : "", info[k].name);
                    }
                }
                jb_add(&j, "]");
            }
            jb_used(&j, known ? gw_filerefs_users(&refs, info[i].name) : 0);
            jb_add(&j, "}");
        }
        size_t fs_total = 0, fs_used = 0;
        gw_storage_fs_info(&fs_total, &fs_used);
        jb_add(&j, "],\"count\":%u,\"total_bytes\":%u,\"max_files\":%d,\"max_file_bytes\":%d,"
               "\"config_known\":%s,\"fs_total_bytes\":%u,\"fs_used_bytes\":%u}\n",
               (unsigned)all, (unsigned)total, GW_PEM_FILES_MAX, GW_PEM_FILE_MAX_BYTES,
               known ? "true" : "false", (unsigned)fs_total, (unsigned)fs_used);
        r = jb_send(req, "200 OK", &j);
    }
    gw_storage_free(n.names);
    gw_storage_free(info);
    gw_storage_free(j.b);
    return r;
}

/* #205 #221: a body that holds a certificate (with its chain) and its private key: the key
 * block goes to @p key, the rest to @p cert (two PSRAM buffers, NUL terminated). False with the
 * reason in @p why when the body has no key block, more than one, or no certificate text. */
static bool split_pair(const char *body, size_t len, char **cert, size_t *clen, char **key,
                       size_t *klen, char *why, size_t why_cap)
{
    size_t off, kn;
    *cert = *key = NULL;
    unsigned blocks = gw_pem_key_blocks(body, len, &off, &kn);
    if (blocks != 1) {
        snprintf(why, why_cap, blocks == 0 ? "the body has no PEM private key: give the "
                                             "certificate and its key"
                                           : "the body has more than one PEM private key");
        return false;
    }
    *cert = gw_storage_malloc(len - kn + 1);
    *key = gw_storage_malloc(kn + 1);
    if (*cert == NULL || *key == NULL) {
        gw_storage_free(*cert);
        gw_storage_free(*key);
        *cert = *key = NULL;
        snprintf(why, why_cap, "no memory");
        return false;
    }
    memcpy(*key, body + off, kn);
    (*key)[kn] = '\0';
    *klen = kn;
    memcpy(*cert, body, off);
    memcpy(*cert + off, body + off + kn, len - off - kn);
    *clen = len - kn;
    (*cert)[*clen] = '\0';
    return true;
}

static void free_pair(char *cert, char *key, size_t klen)
{
    if (key != NULL) {
        memset(key, 0, klen);   /* a private key */
    }
    gw_storage_free(cert);
    gw_storage_free(key);
}

/* An error with a reason longer than GW_CFG_ERR_SIZE (the names of two files in it). */
static esp_err_t send_why(httpd_req_t *req, const char *status, const char *path, const char *why)
{
    char m[2 * 256 + 8], p[2 * GW_CFG_PATH_SIZE], body[sizeof(m) + sizeof(p) + 64];
    json_escape(m, sizeof(m), why);
    json_escape(p, sizeof(p), path);
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\",\"path\":\"%s\"}\n", m, p);
    return send_json(req, status, body);
}

/* The status of an error of gw_pem_load or gw_webcert_put. */
static const char *load_status(esp_err_t e)
{
    return e == ESP_ERR_INVALID_STATE  ? "409 Conflict"
           : e == ESP_ERR_NO_MEM       ? "503 Service Unavailable"
           : e == ESP_ERR_INVALID_SIZE ? "413 Content Too Large"
           : e == ESP_FAIL             ? "507 Insufficient Storage"
           : e == ESP_ERR_INVALID_ARG  ? "400 Bad Request"
                                       : "500 Internal Server Error";
}

/* PUT /api/files?name=<name>[&key_name=<name>][&replace=1][&dry_run=1] with the PEM text: the
 * buttons "Load a file..." of the file fields of the card MQTT, and the load and the replace of
 * the card Certificates. One file, up to GW_PEM_FILE_MAX_BYTES, or with key_name a certificate
 * and its private key together in the body (the gateway separates the key block). Without
 * replace=1, 409 if a file exists. dry_run=1 examines and writes nothing (200). 201 with what
 * each file holds. The configuration does not change: a replaced file that the saved
 * configuration uses applies after the next restart. */
static esp_err_t files_put(httpd_req_t *req)
{
    if (!same_origin(req) || !authorized(req)) {
        return ESP_OK;
    }
    char name[GW_FILE_SIZE + 8], key_name[GW_FILE_SIZE + 8];
    if (!query_get(req, "name", name, sizeof(name)) || name[0] == '\0') {
        return refuse_body(req, "400 Bad Request", "name: give the name of the file (?name=...)");
    }
    bool pair = query_get(req, "key_name", key_name, sizeof(key_name)) && key_name[0] != '\0';
    bool replace = query_one(req, "replace"), dry_run = query_one(req, "dry_run");
    size_t max = (pair ? 2 : 1) * GW_PEM_FILE_MAX_BYTES;
    if (req->content_len > max) {
        char msg[80];
        snprintf(msg, sizeof(msg), "body of %u bytes not accepted (maximum %u)",
                 (unsigned)req->content_len, (unsigned)max);
        return refuse_body(req, "413 Content Too Large", msg);
    }
    size_t len;
    const char *status;
    char *body = read_body(req, &len, &status);
    if (body == NULL) {
        return refuse_body(req, status, "the body is empty or did not arrive");
    }
    gw_pem_load_t *f = gw_storage_calloc(2, sizeof(*f));
    char why[256], path[GW_CFG_PATH_SIZE] = "";
    char *cert = NULL, *key = NULL;
    size_t clen = 0, klen = 0;
    esp_err_t e = ESP_OK;
    size_t n = 1;
    if (f == NULL) {
        snprintf(why, sizeof(why), "no memory");
        e = ESP_ERR_NO_MEM;
    } else if (pair) {
        if (!split_pair(body, len, &cert, &clen, &key, &klen, why, sizeof(why))) {
            e = strcmp(why, "no memory") == 0 ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
        } else {
            f[0] = (gw_pem_load_t){.name = name, .pem = cert, .len = clen};
            f[1] = (gw_pem_load_t){.name = key_name, .pem = key, .len = klen};
            n = 2;
        }
    } else {
        size_t off, kn;
        if (gw_pem_key_blocks(body, len, &off, &kn) > 0 &&
            (off > 0 || kn < len) && strstr(body, "-----BEGIN CERTIFICATE-----") != NULL) {
            snprintf(why, sizeof(why), "%s holds a certificate and a private key: load them "
                     "as a pair (key_name=...)", name);
            e = ESP_ERR_INVALID_ARG;
        }
        f[0] = (gw_pem_load_t){.name = name, .pem = body, .len = len};
    }
    if (e == ESP_OK) {
        e = gw_pem_load(f, n, replace, dry_run, path, sizeof(path), why, sizeof(why));
    }
    esp_err_t r;
    if (e == ESP_OK) {
        jbuf_t j = {.cap = 384 + 2 * (300 + GW_PEM_CERTS_MAX * (2 * GW_PEM_SUBJECT_MAX + 64))};
        j.b = gw_storage_malloc(j.cap);
        if (j.b == NULL) {
            r = send_json(req, dry_run ? "200 OK" : "201 Created", "{\"ok\":true}\n");
        } else {
            jb_add(&j, "{\"ok\":true,\"saved\":%s,\"files\":[", dry_run ? "false" : "true");
            for (size_t i = 0; i < n; i++) {
                jb_add(&j, "%s", i ? "," : "");
                jb_info(&j, &f[i].info);
                jb_add(&j, ",\"replaced\":%s", f[i].exists ? "true" : "false");
                jb_used(&j, f[i].used_by);
                jb_add(&j, "}");
            }
            /* "file": the first file, as before the card Certificates (the file fields) */
            jb_add(&j, "],\"file\":");
            jb_info(&j, &f[0].info);
            jb_add(&j, "}}\n");
            r = jb_send(req, dry_run ? "200 OK" : "201 Created", &j);
            gw_storage_free(j.b);
        }
    } else {
        r = send_why(req, load_status(e), path, why);
    }
    memset(body, 0, len);   /* can be a private key */
    heap_caps_free(body);
    free_pair(cert, key, klen);
    gw_storage_free(f);
    return r;
}

/* DELETE /api/files/<name>: the button Remove of the card Certificates. 409 when the saved
 * configuration names the file (with the keys in "used_by"), or when the saved document cannot
 * be read (nobody can tell which files it uses). 404 when the file is not there. */
static esp_err_t files_delete(httpd_req_t *req)
{
    if (!same_origin(req) || !authorized(req)) {
        return ESP_OK;
    }
    char name[GW_FILE_SIZE + 8];
    const char *p = req->uri + strlen("/api/files/");
    size_t n = strcspn(p, "?#");
    if (n == 0 || n >= sizeof(name)) {
        return send_error(req, "400 Bad Request", NULL, "give the name of the file "
                                                        "(/api/files/<name>)");
    }
    memcpy(name, p, n);
    name[n] = '\0';
    gw_filerefs_t refs;
    if (!gw_storage_file_refs(&refs)) {
        return send_error(req, "409 Conflict", NULL, "the saved configuration cannot be read: "
                                                     "the gateway cannot tell if it uses the "
                                                     "file");
    }
    unsigned used = gw_filerefs_users(&refs, name);
    if (used != 0) {
        char body[400], esc[2 * GW_FILE_SIZE + 8];
        json_escape(esc, sizeof(esc), name);
        int k = snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"the saved configuration "
                         "uses %s: select another file, save, then remove it\",\"path\":\"%s\","
                         "\"used_by\":[", esc, gw_fileref_path((gw_fileref_t)__builtin_ctz(used)));
        unsigned m = 0;
        for (unsigned r = 0; r < GW_FILEREF_COUNT && k > 0 && (size_t)k < sizeof(body); r++) {
            if (used & (1u << r)) {
                k += snprintf(body + k, sizeof(body) - (size_t)k, "%s\"%s\"", m++ ? "," : "",
                              gw_fileref_path((gw_fileref_t)r));
            }
        }
        if (k > 0 && (size_t)k < sizeof(body)) {
            snprintf(body + k, sizeof(body) - (size_t)k, "]}\n");
        }
        return send_json(req, "409 Conflict", body);
    }
    char why[120];
    esp_err_t e = gw_storage_delete_file(name, why, sizeof(why));
    if (e != ESP_OK) {
        return send_error(req, e == ESP_ERR_NOT_FOUND     ? "404 Not Found"
                               : e == ESP_ERR_INVALID_ARG ? "400 Bad Request"
                                                          : "507 Insufficient Storage",
                          NULL, why);
    }
    return send_json(req, "200 OK", "{\"ok\":true,\"removed\":true}\n");
}

/* ---------- #205: the HTTPS certificate of the web UI ---------- */

static void jb_cert(jbuf_t *j, const gw_webcert_info_t *c)
{
    jb_add(j, "{");
    jb_str(j, "source", c->source);
    jb_add(j, ",");
    jb_str(j, "subject", c->subject);
    jb_add(j, ",");
    jb_str(j, "issuer", c->issuer);
    jb_add(j, ",");
    jb_str(j, "sans", c->sans);
    jb_add(j, ",\"not_before\":\"%s\",\"not_after\":\"%s\",\"sha256\":\"%s\",\"chain\":%u,"
           "\"self_signed\":%s,", c->not_before, c->not_after, c->fingerprint, c->chain,
           c->self_signed ? "true" : "false");
    jb_str(j, "key_type", c->key_type);
    jb_add(j, ",\"hostname_in_sans\":%s}",
           gw_san_has_host(c->sans, s_ctx.cfg->network.hostname) ? "true" : "false");
}

/* GET /api/web/certificate: the certificate of this page and the one that waits for the restart
 * ("pending", null when none). Never a key. */
static esp_err_t web_cert_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    gw_webcert_info_t *c = gw_storage_calloc(2, sizeof(*c));
    jbuf_t j = {.cap = 2048 + 2 * sizeof(*c)};
    j.b = gw_storage_malloc(j.cap);
    esp_err_t r;
    if (c == NULL || j.b == NULL) {
        r = send_error(req, "503 Service Unavailable", NULL, "no memory");
    } else {
        bool cur = gw_webcert_get(false, &c[0]), pend = gw_webcert_get(true, &c[1]);
        jb_add(&j, "{\"https\":%s,", s_https ? "true" : "false");
        jb_str(&j, "hostname", s_ctx.cfg->network.hostname);
        jb_add(&j, ",\"current\":");
        if (cur) {
            jb_cert(&j, &c[0]);
        } else {
            jb_add(&j, "null");
        }
        jb_add(&j, ",\"pending\":");
        if (pend) {
            jb_cert(&j, &c[1]);
        } else {
            jb_add(&j, "null");
        }
        jb_add(&j, ",\"max_cert_bytes\":%d,\"max_key_bytes\":%d}\n", GW_WEBCERT_PEM_MAX - 1,
               GW_WEBCERT_KEY_MAX - 1);
        r = jb_send(req, "200 OK", &j);
    }
    gw_storage_free(c);
    gw_storage_free(j.b);
    return r;
}

static esp_err_t send_cert(httpd_req_t *req, const char *status, const char *head,
                           const gw_webcert_info_t *c)
{
    jbuf_t j = {.cap = 1536 + sizeof(*c)};
    j.b = gw_storage_malloc(j.cap);
    if (j.b == NULL) {
        return send_json(req, status, "{\"ok\":true}\n");
    }
    jb_add(&j, "{\"ok\":true,%s,\"certificate\":", head);
    jb_cert(&j, c);
    jb_add(&j, "}\n");
    esp_err_t r = jb_send(req, status, &j);
    gw_storage_free(j.b);
    return r;
}

/* PUT /api/web/certificate[?dry_run=1]: the certificate of the plant (PEM, with its chain) and
 * its unencrypted private key, together in the body. The gateway examines them (the key agrees
 * with the certificate); without dry_run they wait for the restart (201). */
static esp_err_t web_cert_put(httpd_req_t *req)
{
    if (!same_origin(req) || !authorized(req)) {
        return ESP_OK;
    }
    bool dry_run = query_one(req, "dry_run");
    if (req->content_len > GW_WEBCERT_PEM_MAX + GW_WEBCERT_KEY_MAX) {
        char msg[80];
        snprintf(msg, sizeof(msg), "body of %u bytes not accepted (maximum %d)",
                 (unsigned)req->content_len, GW_WEBCERT_PEM_MAX + GW_WEBCERT_KEY_MAX);
        return refuse_body(req, "413 Content Too Large", msg);
    }
    size_t len;
    const char *status;
    char *body = read_body(req, &len, &status);
    if (body == NULL) {
        return refuse_body(req, status, "the body is empty or did not arrive");
    }
    char *cert = NULL, *key = NULL, why[200];
    size_t clen = 0, klen = 0;
    gw_webcert_info_t *c = gw_storage_calloc(1, sizeof(*c));
    esp_err_t e;
    if (c == NULL) {
        snprintf(why, sizeof(why), "no memory");
        e = ESP_ERR_NO_MEM;
    } else if (!split_pair(body, len, &cert, &clen, &key, &klen, why, sizeof(why))) {
        e = strcmp(why, "no memory") == 0 ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
    } else {
        e = gw_webcert_put(cert, clen, key, klen, !dry_run, c, why, sizeof(why));
    }
    memset(body, 0, len);   /* holds a private key */
    heap_caps_free(body);
    free_pair(cert, key, klen);
    esp_err_t r = e == ESP_OK ? send_cert(req, dry_run ? "200 OK" : "201 Created",
                                          dry_run ? "\"saved\":false" : "\"saved\":true,"
                                                                        "\"restart\":true",
                                          c)
                              : send_why(req, load_status(e), "", why);
    gw_storage_free(c);
    return r;
}

/* POST /api/web/certificate/new: a new key and self-signed certificate for the hostname; it
 * waits for the restart (201) and replaces one that waited. */
static esp_err_t web_cert_new_post(httpd_req_t *req)
{
    if (!same_origin(req) || !authorized(req)) {
        return ESP_OK;
    }
    gw_webcert_info_t *c = gw_storage_calloc(1, sizeof(*c));
    char why[200] = "no memory";
    esp_err_t e = c != NULL ? gw_webcert_new(s_ctx.cfg->network.hostname, c, why, sizeof(why))
                            : ESP_ERR_NO_MEM;
    esp_err_t r = e == ESP_OK ? send_cert(req, "201 Created", "\"saved\":true,\"restart\":true", c)
                              : send_error(req, e == ESP_ERR_NO_MEM ? "503 Service Unavailable"
                                                                    : "507 Insufficient Storage",
                                           NULL, why);
    gw_storage_free(c);
    return r;
}

/* DELETE /api/web/certificate/pending: the button Discard. 404 when none waits. */
static esp_err_t web_cert_discard(httpd_req_t *req)
{
    if (!same_origin(req) || !authorized(req)) {
        return ESP_OK;
    }
    esp_err_t e = gw_webcert_discard();
    if (e == ESP_ERR_NOT_FOUND) {
        return send_error(req, "404 Not Found", NULL, "no certificate waits for the restart");
    }
    if (e != ESP_OK) {
        char why[80];
        snprintf(why, sizeof(why), "NVS error %s", esp_err_to_name(e));
        return send_error(req, "507 Insufficient Storage", NULL, why);
    }
    return send_json(req, "200 OK", "{\"ok\":true,\"discarded\":true}\n");
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
    return httpd_resp_send(req, index_html_start, embedded_len(index_html_start, index_html_end));
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

/* #206, #207, #209, #199 and #212: the name and the location of the gateway (not the comment:
 * the decision of #206), the time left of the identify, safe mode, the changed log levels and
 * the health values. Not inlined: its buffer stays off the stack frame of status_get. */
static __attribute__((noinline)) void status_admin(out_t *o, const gw_config_t *c)
{
    outf(o, ",\"gateway\":{\"name\":");
    out_str(o, c->gateway.name);
    outf(o, ",\"location\":");
    out_str(o, c->gateway.location);
    bool requested;
    uint32_t left_s;
    bool safe = gw_fw_safe_mode_info(&requested, &left_s);
    outf(o, "},\"identify_left_s\":%lu,\"safe_mode\":{\"active\":%s,\"cause\":\"%s\","
            "\"left_s\":%lu}",
         (unsigned long)gw_status_identify_left_s(), safe ? "true" : "false",
         !safe ? "" : requested ? "request" : "crashes", (unsigned long)left_s);
    char levels[512];
    gw_log_tags_json(true, levels, sizeof(levels));
    outf(o, ",\"log_levels\":%s", levels);
    float t;
    if (gw_temp_read(&t)) {
        outf(o, ",\"temperature_c\":%.1f", (double)t);
    } else {
        outf(o, ",\"temperature_c\":null");
    }
}

/* NET-5, issue #187: the access point, as an optional object (older clients ignore it).
 * Issue #193: "request_via_ap" = this request came through the access point (the access point
 * rule of the claim window, claim_from_ap: the peer is a host of its subnet), so the web UI can
 * tell that a stop also stops this page. */
static __attribute__((noinline)) void status_ap(out_t *o, httpd_req_t *req)
{
    gw_ap_status_t ap;
    gw_ap_get_status(&ap);
    outf(o, ",\"ap\":{\"available\":%s,\"simulated\":%s,\"running\":%s,\"ssid\":",
         ap.available ? "true" : "false", ap.simulated ? "true" : "false",
         ap.running ? "true" : "false");
    out_str(o, ap.ssid);
    /* the key of the running access point (null while it does not run) */
    outf(o, ",\"security\":%s,\"key\":%s,\"clients\":%u,\"left_s\":",
         !ap.running ? "null" : ap.open ? "\"open\"" : "\"wpa2\"",
         !ap.running ? "null" : ap.key_is_pmk ? "\"web_password\"" : "\"commissioning_password\"",
         ap.clients);
    if (ap.limited) {
        outf(o, "%lu", (unsigned long)ap.left_s);
    } else {
        outf(o, "null");
    }
    outf(o, ",\"open_left_s\":%lu,\"idle_stop_s\":%lu,\"last_stop\":\"%s\","
         "\"request_via_ap\":%s}",
         (unsigned long)ap.open_left_s, (unsigned long)ap.idle_s, gw_ap_stop_key(ap.last_stop),
         claim_from_ap(req) ? "true" : "false");
}

static esp_err_t status_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    static const char *const modes[] = {"normal", "recovery", "commissioning", "fault"};
    out_t *o = out_begin(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const gw_config_t *c = s_ctx.cfg;
    uint32_t f = gw_status_get();
    outf(o, "{\"mode\":\"%s\",\"fault\":", modes[s_ctx.mode]);
    out_str(o, s_ctx.fault != NULL ? s_ctx.fault : "");   /* #135 */
    outf(o, ",\"firmware\":");
    out_str(o, s_ctx.info->firmware);
    outf(o, ",\"reset_reason\":");
    out_str(o, s_ctx.info->reset_reason);
    outf(o, ",\"uptime_s\":%lld,\"config\":{\"ok\":%s,\"source\":",
         esp_timer_get_time() / 1000000, s_ctx.mode == GW_WEB_NORMAL ? "true" : "false");
    out_str(o, s_ctx.config_source);
    outf(o, ",\"error\":");
    out_str(o, s_ctx.config_error != NULL ? s_ctx.config_error->msg : "");
    outf(o, ",\"path\":");
    out_str(o, s_ctx.config_error != NULL ? s_ctx.config_error->path : "");
    outf(o, "},\"network\":{\"up\":%s,\"interfaces\":[", (f & GW_ST_NET_UP) ? "true" : "false");
    gw_net_if_status_t ifs[GW_NET_IF_COUNT];
    gw_net_get_status(ifs);
    static const char *const names[GW_NET_IF_COUNT] = {"ethernet", "wifi"};
    for (int i = 0; i < GW_NET_IF_COUNT; i++) {
        const gw_net_if_status_t *n = &ifs[i];
        outf(o,
             "%s{\"name\":\"%s\",\"configured\":%s,\"link\":%s,\"usable\":%s,\"active\":%s,"
             "\"ipv4\":\"",
             i ? "," : "", names[i], n->configured ? "true" : "false", n->link ? "true" : "false",
             n->usable ? "true" : "false", n->active ? "true" : "false");
        if (n->ipv4[0] | n->ipv4[1] | n->ipv4[2] | n->ipv4[3]) {
            outf(o, "%u.%u.%u.%u", n->ipv4[0], n->ipv4[1], n->ipv4[2], n->ipv4[3]);
        }
        outf(o, "\",\"ipv6\":[");
        bool first = true;
        for (int k = 0; k < GW_NET_IPV6_MAX; k++) {
            if (n->ipv6[k][0] != '\0') {
                outf(o, "%s\"%s\"", first ? "" : ",", n->ipv6[k]);
                first = false;
            }
        }
        outf(o, "]}");
    }
    outf(o, "]},\"mqtt\":{\"connected\":%s}", (f & GW_ST_MQTT_UP) ? "true" : "false");
    if (s_ctx.mode == GW_WEB_NORMAL) {
        outf(o, ",\"sparkplug\":{\"group_id\":");
        out_str(o, c->sparkplug.group_id);
        outf(o, ",\"edge_node_id\":");
        out_str(o, c->sparkplug.edge_node_id);
        outf(o, ",\"diagnostics_s\":%lu}", (unsigned long)c->sparkplug.diagnostics_s);
        gw_log_stats_t ls;
        gw_log_get_stats(&ls);
        if (ls.enabled) {
            char line[200];
            snprintf(line, sizeof(line), "%s:%u %s, sent %lu, dropped %lu, errors %lu",
                     c->syslog.host, c->syslog.port, ls.connected ? "connected" : "not reachable",
                     (unsigned long)ls.sent, (unsigned long)ls.dropped,
                     (unsigned long)ls.send_errors);
            outf(o, ",\"syslog\":");
            out_str(o, line);
        }
    }
    outf(o, ",\"devices\":[");
    for (uint16_t d = 0; s_ctx.mode == GW_WEB_NORMAL && d < c->device_count; d++) {
        const gw_device_cfg_t *dev = &c->devices[d];
        bool online;
        gw_dev_stats_t st;
        gw_tagdb_get_device(c, d, s_vals, &online);
        gw_tagdb_get_stats(d, &st);
        outf(o, "%s{\"name\":", d ? "," : "");
        out_str(o, dev->name);
        outf(o,
             ",\"unit_id\":%u,\"enabled\":%s,\"online\":%s,\"cycles\":%lu,\"failed\":%lu,"
             "\"ok\":%lu,"
             "\"timeout\":%lu,\"crc\":%lu,\"frame\":%lu,\"exception\":%lu,"
             "\"write_refused\":%lu,\"tags\":[",
             dev->unit_id, dev->enabled ? "true" : "false",   /* #214 */
             online ? "true" : "false", (unsigned long)st.cycles,
             (unsigned long)st.failed_cycles, (unsigned long)st.bus.ok,
             (unsigned long)st.bus.timeout, (unsigned long)st.bus.crc,
             (unsigned long)st.bus.frame, (unsigned long)st.bus.exception,
             (unsigned long)gw_tagdb_refused(d));   /* #196 */
        for (uint16_t t = 0; t < dev->tag_count; t++) {
            const gw_tag_value_t *v = &s_vals[t];
            outf(o, "%s{\"name\":", t ? "," : "");
            out_str(o, c->tags[dev->first_tag + t].name);
            const gw_tag_cfg_t *tc = &c->tags[dev->first_tag + t];
            gw_val_kind_t kind = gw_conv_kind(tc);
            /* NaN is not JSON */
            bool valid = v->valid && (kind != GW_VK_DOUBLE || v->value == v->value);
            if (!valid) {
                outf(o, ",\"valid\":false,\"value\":null}");
            } else if (kind == GW_VK_STRING) {
                outf(o, ",\"valid\":true,\"value\":");
                out_str(o, v->str);
                outf(o, "}");
            } else if (kind == GW_VK_BYTES) {   /* SPB-19: hex */
                char hex[2 * GW_STR_MAX + 1];
                gw_conv_format(tc, v, hex, sizeof(hex));
                outf(o, ",\"valid\":true,\"value\":\"%s\"}", hex);
            } else if (kind == GW_VK_INT64) {
                /* exact; #188: also as text, because a JSON reader with doubles (the browser)
                 * rounds a number above 2^53 */
                outf(o, ",\"valid\":true,\"value\":%lld,\"value_text\":\"%lld\"}",
                     (long long)v->i64, (long long)v->i64);
            } else if (kind == GW_VK_UINT64) {
                outf(o, ",\"valid\":true,\"value\":%llu,\"value_text\":\"%llu\"}",
                     (unsigned long long)v->u64, (unsigned long long)v->u64);
            } else {
                /* %.9g: enough for float32 and integers up to 32 bits */
                outf(o, ",\"valid\":true,\"value\":%.9g}", v->value);
            }
        }
        outf(o, "]}");
    }
    gw_local_io_t io;
    gw_tagdb_get_io(&io);
    gw_io_expander_t exp;
    gw_io_get_expander(&exp);
    /* issue #74: unknown DOs (expander not answering) are null, never "off" */
    outf(o, "],\"io\":{\"di\":%u,\"do\":", io.di);
    if (io.do_unknown) {
        outf(o, "null");
    } else {
        outf(o, "%u", io.dout);
    }
    outf(o, ",\"do_unknown\":%s,\"expander\":{\"ready\":%s,\"retrying\":%s,\"attempts\":%u}",
         io.do_unknown ? "true" : "false", exp.ready ? "true" : "false",
         exp.retrying ? "true" : "false", exp.attempts);
    /* issue #75: what IO-3, IO-4, IO-6 and IO-7 added */
    outf(o, ",\"failsafe\":%s,\"counters\":[", io.failsafe ? "true" : "false");
    for (int k = 0; k < GW_LOCAL_IO_CHANNELS; k++) {
        outf(o, "%s%llu", k ? "," : "", (unsigned long long)io.count[k]);
    }
    uint8_t rule_do = 0;
    for (uint8_t r = 0; s_ctx.mode == GW_WEB_NORMAL && r < c->local_io.rule_count; r++) {
        rule_do |= (uint8_t)(1u << c->local_io.rules[r].dout);
    }
    outf(o, "],\"rule_do\":%u", rule_do);
    /* #204 #210 #217 #218: the local rules (DI and mode of each DO with a rule), the pulse time
     * of each DO, the fast counters, and the run hours (s; null for a channel that does not
     * count them) */
    bool normal = s_ctx.mode == GW_WEB_NORMAL;
    static const char *const rule_modes[] = {"follow", "invert", "toggle"};
    outf(o, ",\"rules\":[");
    for (uint8_t r = 0; normal && r < c->local_io.rule_count; r++) {
        outf(o, "%s{\"di\":%u,\"do\":%u,\"mode\":\"%s\"}", r ? "," : "",
             c->local_io.rules[r].di + 1u, c->local_io.rules[r].dout + 1u,
             rule_modes[c->local_io.rules[r].mode % 3]);
    }
    outf(o, "],\"pulse_ms\":[");
    for (int k = 0; k < GW_LOCAL_IO_CHANNELS; k++) {
        outf(o, "%s%u", k ? "," : "", normal ? c->local_io.do_pulse_ms[k] : 0u);
    }
    outf(o, "],\"fast\":%u,\"run_hours\":{", normal ? c->local_io.fast_mask : 0u);
    for (int g = 0; g < 2; g++) {
        outf(o, "%s\"%s\":[", g ? "," : "", g ? "do" : "di");
        for (int k = 0; k < GW_LOCAL_IO_CHANNELS; k++) {
            int ch = g * GW_LOCAL_IO_CHANNELS + k;
            if (normal && (c->local_io.run_hours & (1u << ch))) {
                outf(o, "%s%llu", k ? "," : "", (unsigned long long)io.run_s[ch]);
            } else {
                outf(o, "%snull", k ? "," : "");
            }
        }
        outf(o, "]");
    }
    outf(o, "}}");
    static const char *const alarm_keys[] = {"none", "device_offline", "mqtt_down", "config_error"};
    gw_alarm_t alarm = gw_buzzer_alarm();
    uint8_t enabled = gw_buzzer_alarms();
    outf(o, ",\"buzzer\":{\"alarm\":\"%s\",\"enabled\":[", alarm_keys[alarm & 3]);
    bool first_alarm = true;
    for (int a = GW_ALARM_DEVICE_OFFLINE; a <= GW_ALARM_CONFIG_ERROR; a++) {
        if (enabled & GW_ALARM_BIT(a)) {
            outf(o, "%s\"%s\"", first_alarm ? "" : ",", alarm_keys[a]);
            first_alarm = false;
        }
    }
    /* TIME-1..3 */
    gw_io_rtc_info_t rtc;
    gw_io_rtc_get(&rtc);
    gw_clock_source_t src = gw_edge_time_source();   /* #160: "valid", source "browser" */
    /* #160: "waiting": normal mode, and the MQTT session waits for a valid clock; rtc.lost: the
     * OS bit was set at boot (examine the battery) */
    outf(o, "]},\"time\":{\"utc_s\":%lld,\"synced\":%s,\"valid\":%s,\"source\":\"%s\","
             "\"waiting\":%s,\"rtc\":{\"present\":%s,\"lost\":%s,\"status\":",
         (long long)time(NULL), src == GW_CLOCK_SNTP ? "true" : "false",
         gw_clock_valid(src) ? "true" : "false", gw_clock_source_key(src),
         s_ctx.mode == GW_WEB_NORMAL && !gw_clock_valid(src) ? "true" : "false",
         rtc.present ? "true" : "false", rtc.lost ? "true" : "false");
    out_str(o, rtc.present ? gw_rtc_status_str((gw_rtc_status_t)rtc.status) : "not answering");
    outf(o, ",\"stores\":%lu}}", (unsigned long)rtc.stores);
    status_card(o, c);
    status_ap(o, req);
    status_admin(o, c);
    {   /* #132: is a core dump in flash (no flash access; the summary is a route of its own) */
        uint32_t size;
        gw_cdump_state_t cd = gw_coredump_state(&size);
        char buf[80];
        gw_cdump_status_json(cd, size, buf, sizeof(buf));
        outf(o, ",\"core_dump\":%s", buf);
    }
    outf(o,
         ",\"heap\":{\"internal_free\":%u,\"internal_min\":%u,\"internal_largest\":%u,"
         "\"psram_free\":%u}}\n",
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
         (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),   /* #212 */
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    out_flush(o);
    return o->err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : o->err;
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
    hc.uri_match_fn = httpd_uri_match_wildcard;   /* #221: DELETE /api/files/<name> */
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
                           h_claim_get = {claim_get}, h_claim_post = {claim_post},
                           h_ap_stop = {ap_stop_post}, h_identify = {identify_post},
                           h_factory_reset = {factory_reset_post}, h_log = {log_get},
                           h_log_level_get = {log_level_get},
                           h_log_level_post = {log_level_post}, h_do = {do_post},
                           h_wifi_scan = {wifi_scan_get}, h_files_get = {files_get},
                           h_files_put = {files_put}, h_files_delete = {files_delete},
                           h_cert_get = {web_cert_get}, h_cert_put = {web_cert_put},
                           h_cert_new = {web_cert_new_post}, h_cert_discard = {web_cert_discard};
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
        {"/api/ap/stop", HTTP_POST, &h_ap_stop},           /* #187 */
        {"/api/identify", HTTP_POST, &h_identify},         /* #207 */
        {"/api/factory-reset", HTTP_POST, &h_factory_reset},   /* #203 */
        {"/api/log", HTTP_GET, &h_log},                    /* #200 */
        {"/api/log/level", HTTP_GET, &h_log_level_get},    /* #199 */
        {"/api/log/level", HTTP_POST, &h_log_level_post},
        {"/api/do", HTTP_POST, &h_do},                     /* #204 */
        {"/api/wifi/scan", HTTP_GET, &h_wifi_scan},        /* #208 */
        {"/api/files", HTTP_GET, &h_files_get},            /* #221 */
        {"/api/files", HTTP_PUT, &h_files_put},
        {"/api/files/*", HTTP_DELETE, &h_files_delete},
        {"/api/web/certificate", HTTP_GET, &h_cert_get},    /* #205 */
        {"/api/web/certificate", HTTP_PUT, &h_cert_put},
        {"/api/web/certificate/new", HTTP_POST, &h_cert_new},
        {"/api/web/certificate/pending", HTTP_DELETE, &h_cert_discard},
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
