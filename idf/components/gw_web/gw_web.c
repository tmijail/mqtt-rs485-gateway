#include "gw_web.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "gw_auth.h"
#include "gw_log.h"
#include "gw_net.h"
#include "gw_status.h"
#include "gw_storage.h"
#include "gw_tagdb.h"

static const char *TAG = "web";

#define REALM_HDR "Basic realm=\"gateway\", charset=\"UTF-8\""

static gw_web_ctx_t s_ctx;
static httpd_handle_t s_server;

/* Credentials checked by the server task only (single task: no lock needed). */
static char s_user[GW_USER_SIZE];           /* "" = any user name (recovery) */
static char s_hash[GW_PWHASH_SIZE];
static uint8_t s_ok_digest[GW_SHA256_LEN];   /* SHA-256 of the last accepted Authorization */
static bool s_ok_cached;

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

static bool authorized(httpd_req_t *req)
{
    char hdr[192];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK) {
        /* PBKDF2 costs tens of ms: a browser repeats the same header on every request, so the
         * digest of the last accepted one is remembered. */
        uint8_t digest[GW_SHA256_LEN];
        gw_sha256(hdr, strlen(hdr), digest);
        if (s_ok_cached && gw_auth_equal(digest, s_ok_digest, sizeof(digest))) {
            return true;
        }
        if (check_credentials(hdr)) {
            memcpy(s_ok_digest, digest, sizeof(digest));
            s_ok_cached = true;
            return true;
        }
        ESP_LOGW(TAG, "%s %s: wrong credentials", req->method == HTTP_GET ? "GET" : "write",
                 req->uri);
    }
    httpd_resp_set_hdr(req, "WWW-Authenticate", REALM_HDR);
    send_error(req, "401 Unauthorized", NULL, "autenticación requerida");
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
    if (!authorized(req)) {
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
    if (!authorized(req)) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "restart requested through the REST API");
    send_json(req, "202 Accepted", "{\"ok\":true,\"restart\":true}\n");
    s_ctx.restart();
    return ESP_OK;
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
        outf(&o, "}");
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
            /* %.9g: enough for float32 and integers up to 32 bits; NaN is not JSON */
            bool valid = v->valid && v->value == v->value;
            if (valid) {
                outf(&o, ",\"valid\":true,\"value\":%.9g}", v->value);
            } else {
                outf(&o, ",\"valid\":false,\"value\":null}");
            }
        }
        outf(&o, "]}");
    }
    gw_local_io_t io;
    gw_tagdb_get_io(&io);
    outf(&o,
         "],\"io\":{\"di\":%u,\"do\":%u},\"heap\":{\"internal_free\":%u,\"internal_min\":%u,"
         "\"psram_free\":%u}}\n",
         io.di, io.dout, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
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

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.server_port = ctx->cfg->web.port;
    hc.task_priority = 3;
    hc.stack_size = 8192;            /* configuration validation runs in this task */
    hc.max_uri_handlers = 12;
    hc.max_open_sockets = 4;
    hc.lru_purge_enable = true;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &hc), TAG, "httpd_start");

    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/config", .method = HTTP_GET, .handler = config_get},
        {.uri = "/api/config", .method = HTTP_PUT, .handler = config_put},
        {.uri = "/api/config", .method = HTTP_POST, .handler = config_put},
        {.uri = "/api/restart", .method = HTTP_POST, .handler = restart_post},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &uris[i]), TAG, "%s",
                            uris[i].uri);
    }
    ESP_LOGI(TAG, "web UI and REST API on port %u (%s)", hc.server_port,
             s_hash[0] == '\0' ? "user 'admin' with the commissioning password: no web password set"
             : s_user[0] != '\0' ? "web.username and web.password"
                                : "stored web password, any user name");
    return ESP_OK;
}
