#include "gw_web.h"

#include <stdarg.h>
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
#include "gw_auth.h"
#include "gw_authlimit.h"
#include "gw_fw.h"
#include "gw_buzz.h"
#include "gw_buzzer.h"
#include "gw_conv.h"
#include "gw_edge.h"
#include "gw_io.h"
#include "gw_rtc.h"
#include "gw_log.h"
#include "gw_net.h"
#include "gw_ota.h"
#include "gw_status.h"
#include "gw_storage.h"
#include "gw_tagdb.h"
#include "gw_webcert.h"
#include "lwip/sockets.h"

static const char *TAG = "web";

#define REALM_HDR "Basic realm=\"gateway\", charset=\"UTF-8\""

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
        static char retry[12];   /* server task only; must outlive the response */
        snprintf(retry, sizeof(retry), "%lu", (unsigned long)((left + 999) / 1000));
        httpd_resp_set_hdr(req, "Retry-After", retry);
        send_error(req, "429 Too Many Requests", NULL,
                   "demasiados intentos de acceso fallidos: espere y vuelva a intentar");
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
    send_error(req, "401 Unauthorized", NULL, "autenticación requerida");
    return false;
}

/* Value of header @p name in @p buf ("" when absent). False if it does not fit. */
static bool header(httpd_req_t *req, const char *name, char *buf, size_t cap)
{
    buf[0] = '\0';
    size_t n = httpd_req_get_hdr_value_len(req, name);
    return n == 0 || (n < cap && httpd_req_get_hdr_value_str(req, name, buf, cap) == ESP_OK);
}

/* Issue #100: a state-changing request (PUT, POST) must carry GW_CSRF_HEADER and, from a
 * browser, come from this same origin (gw_auth_csrf_check). Checked before the credentials:
 * a forged request is refused without a password check, so it neither succeeds with the
 * browser's cached credentials nor counts as a failed login against the operator's address. */
static bool same_origin(httpd_req_t *req)
{
    static char host[96], origin[128], referer[256];   /* server task only */
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
    ESP_LOGW(TAG, "%s %s from %s refused: %s", req->method == HTTP_PUT ? "PUT" : "POST", req->uri,
             who, r == GW_CSRF_NO_HEADER ? "no " GW_CSRF_HEADER " header" : "cross-origin request");
    httpd_resp_set_hdr(req, "Connection", "close");   /* its body (an image?) is not read */
    send_error(req, "403 Forbidden", NULL,
               r == GW_CSRF_NO_HEADER
                   ? "falta la cabecera " GW_CSRF_HEADER ": las escrituras la necesitan (CSRF)"
                   : "solicitud de otro origen rechazada (Origin o Referer no es este equipo)");
    return false;
}

static bool query_flag(httpd_req_t *req, const char *key)
{
    char q[64], v[8];
    return httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
           httpd_query_key_value(q, key, v, sizeof(v)) == ESP_OK && strcmp(v, "0") != 0;
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
    char *buf = heap_caps_malloc_prefer(req->content_len + 1, 2,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
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

static esp_err_t config_put(httpd_req_t *req)
{
    if (!same_origin(req) || !authorized(req)) {
        return ESP_OK;
    }
    size_t len;
    const char *status;
    char *body = read_body(req, &len, &status);
    if (body == NULL) {
        char msg[96];
        snprintf(msg, sizeof(msg), "cuerpo de %u bytes no aceptado (máximo %d)",
                 (unsigned)req->content_len, CONFIG_GW_CONFIG_MAX_BYTES);
        httpd_resp_set_hdr(req, "Connection", "close");
        return send_error(req, status, NULL, msg);
    }
    bool dry_run = query_flag(req, "dry_run");
    gw_cfg_err_t err;
    esp_err_t r = gw_storage_save_config(body, len, dry_run, &err);
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
    default:
        return send_error(req, "500 Internal Server Error", &err, NULL);
    }
    if (dry_run) {
        return send_json(req, "200 OK", "{\"ok\":true,\"saved\":false}\n");
    }
    ESP_LOGW(TAG, "new configuration saved through the REST API: restarting");
    send_json(req, "200 OK", "{\"ok\":true,\"saved\":true,\"restart\":true}\n");
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

/* ---------- FW-1: /api/firmware ---------- */

static const char *const k_fw_phase[] = {"idle", "downloading", "rebooting", "failed", "trial",
                                         "confirmed", "rolled_back"};

static esp_err_t firmware_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    /* about 3 KB for one request: from the heap (PSRAM when present), not the server task's
     * stack nor .bss (issue #80: internal RAM is tight without PSRAM) */
    typedef struct {
        gw_fw_status_t f;
        char text[2 * sizeof(((gw_fw_status_t *)0)->text)];
        char detail[2 * sizeof(((gw_fw_status_t *)0)->detail)];
        char run[2 * sizeof(((gw_fw_status_t *)0)->running)];
        char other[2 * sizeof(((gw_fw_status_t *)0)->other)];
        char body[1400];
    } fw_json_t;
    fw_json_t *j = heap_caps_malloc_prefer(sizeof(*j), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                           MALLOC_CAP_8BIT);
    if (j == NULL) {
        return send_error(req, "503 Service Unavailable", NULL, "sin memoria");
    }
    gw_fw_get_status(&j->f);
    json_escape(j->text, sizeof(j->text), j->f.text);
    json_escape(j->detail, sizeof(j->detail), j->f.detail);
    json_escape(j->run, sizeof(j->run), j->f.running);
    json_escape(j->other, sizeof(j->other), j->f.other);
    snprintf(j->body, sizeof(j->body),
             "{\"running\":\"%s\",\"slot\":\"%s\",\"other\":\"%s\",\"enabled\":%s,"
             "\"phase\":\"%s\",\"state\":\"%s\",\"detail\":\"%s\",\"progress\":%u,"
             "\"bytes\":%lu,\"total\":%lu,\"trial_left_s\":%lu}\n",
             j->run, j->f.slot, j->other, j->f.enabled ? "true" : "false",
             j->f.phase < sizeof(k_fw_phase) / sizeof(k_fw_phase[0]) ? k_fw_phase[j->f.phase] : "?",
             j->text, j->detail, j->f.progress, (unsigned long)j->f.bytes,
             (unsigned long)j->f.total, (unsigned long)j->f.trial_left_s);
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
 * content type is refused (issue #100: text/plain is what a foreign page can send). */
static esp_err_t firmware_post(httpd_req_t *req)
{
    if (!same_origin(req) || !authorized(req)) {
        return ESP_OK;
    }
    char ctype[48] = "";
    httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof(ctype));
    char why[160];
    bool json = media_type_is(ctype, "application/json");
    if (!json && !media_type_is(ctype, "application/octet-stream")) {
        httpd_resp_set_hdr(req, "Connection", "close");
        return send_error(req, "415 Unsupported Media Type", NULL,
                          "Content-Type: application/json ({\"url\": ...}) o "
                          "application/octet-stream (la imagen)");
    }
    if (json) {
        size_t len;
        const char *status;
        char *body = req->content_len <= 1024 ? read_body(req, &len, &status) : NULL;
        if (body == NULL) {
            httpd_resp_set_hdr(req, "Connection", "close");
            return send_error(req, "400 Bad Request", NULL, "se esperaba {\"url\": \"https://...\"}");
        }
        char url[GW_OTA_URL_MAX];
        bool parsed = gw_ota_parse_request(body, len, url, why, sizeof(why));
        heap_caps_free(body);
        if (!parsed) {
            return send_error(req, "400 Bad Request", NULL, why);
        }
        esp_err_t e = gw_fw_start_url(url, why, sizeof(why));
        if (e != ESP_OK) {
            return send_error(req, e == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "409 Conflict",
                              NULL, why);
        }
        ESP_LOGW(TAG, "firmware update from %s requested through the REST API", url);
        return send_json(req, "202 Accepted", "{\"ok\":true,\"started\":true}\n");
    }
    if (req->content_len == 0) {
        return send_error(req, "411 Length Required", NULL, "imagen sin Content-Length");
    }
    esp_err_t e = gw_fw_upload_begin(req->content_len, why, sizeof(why));
    if (e != ESP_OK) {
        httpd_resp_set_hdr(req, "Connection", "close");
        return send_error(req, e == ESP_ERR_INVALID_SIZE ? "413 Content Too Large" : "409 Conflict",
                          NULL, why);
    }
    ESP_LOGW(TAG, "firmware upload of %u bytes through the REST API", (unsigned)req->content_len);
    const size_t chunk = 4096;
    char *buf = heap_caps_malloc_prefer(chunk, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                        MALLOC_CAP_8BIT);
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
        if (gw_fw_upload_write(buf, (size_t)r, why, sizeof(why)) != ESP_OK) {
            heap_caps_free(buf);
            httpd_resp_set_hdr(req, "Connection", "close");
            return send_error(req, "400 Bad Request", NULL, why);
        }
        got += (size_t)r;
    }
    heap_caps_free(buf);
    e = gw_fw_upload_end(ok && got == req->content_len, why, sizeof(why));
    if (e != ESP_OK) {
        httpd_resp_set_hdr(req, "Connection", "close");
        return send_error(req, !ok ? "503 Service Unavailable"
                               : got < req->content_len ? "408 Request Timeout" : "400 Bad Request",
                          NULL, why);
    }
    return send_json(req, "200 OK", "{\"ok\":true,\"restart\":true}\n");
}

/* ---------- / and /api/status ---------- */

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static esp_err_t index_get(httpd_req_t *req)
{
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

static esp_err_t status_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    static const char *const modes[] = {"normal", "recovery", "commissioning"};
    static out_t o;   /* server task only */
    o.req = req;
    o.len = 0;
    o.err = ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const gw_config_t *c = s_ctx.cfg;
    uint32_t f = gw_status_get();
    outf(&o, "{\"mode\":\"%s\",\"firmware\":", modes[s_ctx.mode]);
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
    bool synced = gw_edge_time_synced();
    outf(&o, "]},\"time\":{\"utc_s\":%lld,\"synced\":%s,\"source\":\"%s\",\"rtc\":{"
             "\"present\":%s,\"status\":",
         (long long)time(NULL), synced ? "true" : "false",
         synced ? "sntp" : rtc.restored ? "rtc" : "none", rtc.present ? "true" : "false");
    out_str(&o, rtc.present ? gw_rtc_status_str((gw_rtc_status_t)rtc.status) : "not answering");
    outf(&o, ",\"stores\":%lu}", (unsigned long)rtc.stores);
    outf(&o,
         "},\"heap\":{\"internal_free\":%u,\"internal_min\":%u,"
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
    /* 301 for GET; 308 keeps the method and body of a PUT/POST */
    httpd_resp_set_status(req, req->method == HTTP_GET ? "301 Moved Permanently"
                                                       : "308 Permanent Redirect");
    httpd_resp_set_hdr(req, "Location", loc);
    httpd_resp_set_hdr(req, "Connection", "close");
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
    hc.max_uri_handlers = 3;
    hc.uri_match_fn = httpd_uri_match_wildcard;
    ESP_RETURN_ON_ERROR(httpd_start(&s_redirect, &hc), TAG, "redirect server");
    static const httpd_method_t methods[] = {HTTP_GET, HTTP_PUT, HTTP_POST};
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        const httpd_uri_t u = {.uri = "/*", .method = methods[i], .handler = redirect_any};
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_redirect, &u), TAG, "redirect uri");
    }
    return ESP_OK;
}

void gw_web_get_stats(gw_web_stats_t *out)
{
    out->running = s_server != NULL;
    out->https = s_https;
    out->failed = s_limit.failed_total;
    out->refused = s_limit.refused_total;
    out->blocks = s_limit.blocks_total;
    out->csrf_refused = s_csrf_refused;
    out->blocked_now = gw_authlimit_blocked(&s_limit, (uint64_t)(esp_timer_get_time() / 1000));
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

    gw_authlimit_init(&s_limit, CONFIG_GW_WEB_LOGIN_MAX_FAILS, CONFIG_GW_WEB_LOGIN_BLOCK_S * 1000u,
                      CONFIG_GW_WEB_LOGIN_BLOCK_MAX_S * 1000u);
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.server_port = ctx->cfg->web.port;
    hc.task_priority = 3;
    hc.stack_size = 8192;            /* configuration validation runs in this task */
    hc.max_uri_handlers = 12;
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
         * from PSRAM when the board has it (gw_storage_init) */
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

    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/config", .method = HTTP_GET, .handler = config_get},
        {.uri = "/api/config", .method = HTTP_PUT, .handler = config_put},
        {.uri = "/api/config", .method = HTTP_POST, .handler = config_put},
        {.uri = "/api/restart", .method = HTTP_POST, .handler = restart_post},
        {.uri = "/api/firmware", .method = HTTP_GET, .handler = firmware_get},     /* FW-1 */
        {.uri = "/api/firmware", .method = HTTP_POST, .handler = firmware_post},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &uris[i]), TAG, "%s",
                            uris[i].uri);
    }
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
