#include "gw_shell.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_bootbtn.h"
#include "gw_bus.h"
#include "gw_cfgdoc.h"
#include "gw_edge.h"
#include "gw_io.h"
#include "gw_log.h"
#include "gw_net.h"
#include "gw_status.h"
#include "gw_storage.h"
#include "gw_tagdb.h"
#include "gw_web.h"
#include "lwip/ip_addr.h"
#include "ping/ping_sock.h"

static const char *TAG = "shell";

static gw_shell_ctx_t s_ctx;
static gw_tag_value_t s_vals[GW_MAX_TAGS_PER_DEVICE];   /* scratch for gw_tagdb_get_device */

/* ---------- answers atomic with respect to the log (#56) ----------
 * The console UART carries the log of every task and the shell's answers. A command builds its
 * answer in s_out and writes it with one fwrite under s_console, which the log output also
 * takes (log_vprintf, installed as esp_log's vprintf): a log line can come before or after an
 * answer, never inside one of its lines. Long answers go out in pieces of whole lines. */
static SemaphoreHandle_t s_console;
static StaticSemaphore_t s_console_buf;
static vprintf_like_t s_prev_vprintf;
static char s_out[1024];
static size_t s_out_len;

#define CONSOLE_LOCK_MS 500   /* a log line never waits longer than this for an answer */

static int log_vprintf(const char *fmt, va_list ap)
{
    bool locked = !xPortInIsrContext() &&
                  xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
                  xSemaphoreTakeRecursive(s_console, pdMS_TO_TICKS(CONSOLE_LOCK_MS)) == pdTRUE;
    int r = s_prev_vprintf(fmt, ap);
    if (locked) {
        xSemaphoreGiveRecursive(s_console);
    }
    return r;
}

static void write_locked(const char *data, size_t len)
{
    xSemaphoreTakeRecursive(s_console, portMAX_DELAY);
    fwrite(data, 1, len, stdout);
    fflush(stdout);
    xSemaphoreGiveRecursive(s_console);
}

static void out_flush(void)
{
    if (s_out_len > 0) {
        write_locked(s_out, s_out_len);
        s_out_len = 0;
    }
}

/* Make room: write the complete lines buffered so far, keep a started line. */
static void out_make_room(void)
{
    size_t keep = 0;
    while (keep < s_out_len && s_out[s_out_len - 1 - keep] != '\n') {
        keep++;
    }
    if (keep == s_out_len) {   /* a single line longer than the buffer: write it as it is */
        out_flush();
        return;
    }
    write_locked(s_out, s_out_len - keep);
    memmove(s_out, s_out + s_out_len - keep, keep);
    s_out_len = keep;
}

static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void out(const char *fmt, ...)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(s_out + s_out_len, sizeof(s_out) - s_out_len, fmt, ap);
        va_end(ap);
        if (n >= 0 && (size_t)n < sizeof(s_out) - s_out_len) {
            s_out_len += (size_t)n;
            return;
        }
        if (attempt == 0) {
            out_make_room();
        } else {
            s_out_len = sizeof(s_out) - 1;   /* truncated */
        }
    }
}

/* Large text (config show): copied in pieces through the buffer. */
static void out_raw(const char *data, size_t len)
{
    while (len > 0) {
        if (s_out_len == sizeof(s_out)) {
            out_make_room();
        }
        size_t n = sizeof(s_out) - s_out_len;
        n = n < len ? n : len;
        memcpy(s_out + s_out_len, data, n);
        s_out_len += n;
        data += n;
        len -= n;
    }
}

/* Every command runs through here: its answer leaves in one piece when it returns. */
#define ATOMIC(fn)                                    \
    static int fn##_atomic(int argc, char **argv)     \
    {                                                 \
        s_out_len = 0;                                \
        int rc = fn(argc, argv);                      \
        out_flush();                                  \
        return rc;                                    \
    }

static bool device_online(uint16_t dev)
{
    bool online;
    gw_tagdb_get_device(s_ctx.cfg, dev, s_vals, &online);
    return online;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint32_t f = gw_status_get();
    out("firmware %s, reset reason %s, uptime %lld s\n", s_ctx.info->firmware,
           s_ctx.info->reset_reason, esp_timer_get_time() / 1000000);
    if (s_ctx.cfg == NULL && s_ctx.commissioning) {
        out("config   NONE: commissioning after a factory reset; load one with the web UI or "
               "'config load'\n");
        out("         nothing is running: no polling, no MQTT; network and web UI only\n");
        out("network  %s\n", (f & GW_ST_NET_UP) ? "up" : "down");
    } else if (s_ctx.cfg == NULL) {
        out("config   REJECTED (%s): %s\n", s_ctx.config_source, s_ctx.config_error);
        out("         nothing is running: no polling, no MQTT; recovery network and web UI "
               "only\n");
        out("network  %s\n", (f & GW_ST_NET_UP) ? "up" : "down");
    } else {
        uint16_t offline = 0;
        for (uint16_t d = 0; d < s_ctx.cfg->device_count; d++) {
            offline = (uint16_t)(offline + (device_online(d) ? 0u : 1u));
        }
        out("config   %s: ok\n", s_ctx.config_source);
        out("network  %s, MQTT %s, Modbus %u/%u devices online\n",
               (f & GW_ST_NET_UP) ? "up" : "down",
               (f & GW_ST_MQTT_UP) ? "connected" : "disconnected",
               s_ctx.cfg->device_count - offline, s_ctx.cfg->device_count);
        gw_edge_status_t es;
        gw_edge_get_status(&es);
        if (es.host != NULL) {
            out("mqtt     broker %u/%u %s:%u%s, %u failed attempt(s) in a row\n", es.broker + 1u,
                es.broker_count, es.host, es.port, s_ctx.cfg->mqtt.tls.enabled ? " (TLS)" : "",
                es.failures);
        }
        if (es.sf_enabled) {
            out("store    %lu of %lu samples waiting (%s), %lu historical messages sent, %lu "
                "overwritten\n",
                (unsigned long)es.sf_count, (unsigned long)es.sf_cap,
                es.sf_psram ? "PSRAM" : "internal RAM", (unsigned long)es.sf_sent,
                (unsigned long)es.sf_dropped);
        }
        if (s_ctx.cfg->sparkplug.primary_host_id[0] != '\0') {
            out("host     primary '%s' %s%s\n", s_ctx.cfg->sparkplug.primary_host_id,
                es.host_state < 0 ? "not seen yet" : es.host_state ? "online" : "offline",
                es.waiting_host ? ", NBIRTH waits for it" : "");
        }
    }
    gw_net_if_status_t ifs[GW_NET_IF_COUNT];
    gw_net_get_status(ifs);
    static const char *const names[GW_NET_IF_COUNT] = {"ethernet", "wifi"};
    out("iface    ");
    for (int i = 0; i < GW_NET_IF_COUNT; i++) {
        const gw_net_if_status_t *n = &ifs[i];
        out("%s%s", i ? " | " : "", names[i]);
        if (!n->configured) {
            out(" not configured");
            continue;
        }
        out(" %s", !n->link ? "link down" : n->usable ? "up" : "link up, no address");
        if (n->usable) {
            out(" %u.%u.%u.%u", n->ipv4[0], n->ipv4[1], n->ipv4[2], n->ipv4[3]);
        }
        if (n->active) {
            out(" (active)");
        }
    }
    out("\n");
    bool any6 = false;
    for (int i = 0; i < GW_NET_IF_COUNT; i++) {
        for (int k = 0; k < GW_NET_IPV6_MAX; k++) {
            if (ifs[i].ipv6[k][0] != '\0') {
                out("%s%s %s", any6 ? ", " : "ipv6     ", names[i], ifs[i].ipv6[k]);
                any6 = true;
            }
        }
    }
    if (any6) {
        out("\n");
    }
    gw_web_stats_t ws;
    gw_web_get_stats(&ws);
    if (ws.running) {
        out("web      %s, logins failed %lu, refused while blocked %lu, blocks %lu, addresses "
            "blocked now %u\n",
            ws.https ? "HTTPS (HTTP redirects)" : "HTTP", (unsigned long)ws.failed,
            (unsigned long)ws.refused, (unsigned long)ws.blocks, ws.blocked_now);
    }
    gw_log_stats_t ls;
    gw_log_get_stats(&ls);
    if (ls.enabled && s_ctx.cfg != NULL) {
        out("syslog   %s:%u %s, sent %lu, dropped %lu, errors %lu\n", s_ctx.cfg->syslog.host,
               s_ctx.cfg->syslog.port, ls.connected ? "connected" : "not reachable",
               (unsigned long)ls.sent, (unsigned long)ls.dropped, (unsigned long)ls.send_errors);
    }
#if CONFIG_NVS_ENCRYPTION && CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC
    out("secrets  NVS %s (HMAC key in eFuse KEY%d)\n",
           gw_storage_secrets_encrypted() ? "encrypted" : "UNAVAILABLE",
           CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID);
#else
    out("secrets  NVS NOT encrypted (default build; sdkconfig.production encrypts it)\n");
#endif
    gw_local_io_t io;
    gw_tagdb_get_io(&io);
    if (io.do_unknown) {
        out("local    DI 0x%02x, DO unknown: output expander not answering (retrying)\n", io.di);
    } else {
        out("local    DI 0x%02x, DO 0x%02x (bit n = channel n+1, 1 = active)\n", io.di, io.dout);
    }
    /* Core of each task that matters for timing ("-" = no affinity; tasks not running, such as
     * the Ethernet RX task of the other MAC, are omitted). */
    static const char *const tasks[] = {"rs485_bus", "sparkplug", "tcpip", "mqtt_task",
                                        "w5500_tsk", "emac_rx",   "wifi",      "sys_evt"};
    out("tasks   ");
    for (size_t i = 0; i < sizeof(tasks) / sizeof(tasks[0]); i++) {
        TaskHandle_t h = xTaskGetHandle(tasks[i]);
        if (h == NULL) {
            continue;
        }
        BaseType_t core = xTaskGetCoreID(h);
        if (core == tskNO_AFFINITY) {
            out(" %s -", tasks[i]);
        } else {
            out(" %s %d", tasks[i], (int)core);
        }
    }
    out(" (core)\n");
    out("heap     internal %u B free (minimum %u B), PSRAM %u B free\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return 0;
}

static int cmd_devices(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (s_ctx.cfg == NULL) {
        out("no configuration\n");
        return 1;
    }
    for (uint16_t d = 0; d < s_ctx.cfg->device_count; d++) {
        const gw_device_cfg_t *dev = &s_ctx.cfg->devices[d];
        gw_dev_stats_t st;
        gw_tagdb_get_stats(d, &st);
        out("%-16s unit %3u %-7s cycles %lu failed %lu | ok %lu timeout %lu crc %lu frame %lu "
               "exception %lu\n",
               dev->name, dev->unit_id, device_online(d) ? "online" : "offline",
               (unsigned long)st.cycles, (unsigned long)st.failed_cycles, (unsigned long)st.bus.ok,
               (unsigned long)st.bus.timeout, (unsigned long)st.bus.crc,
               (unsigned long)st.bus.frame, (unsigned long)st.bus.exception);
    }
    return 0;
}

static int config_show(void)
{
    size_t len;
    gw_cfg_err_t err;
    char *doc = gw_storage_export_config(&len, &err);
    if (doc == NULL) {
        out("cannot export: %s\n", err.msg);
        return 1;
    }
    out_raw(doc, len);
    gw_storage_free(doc);
    return 0;
}

#if CONFIG_GW_SHELL_WRITE_COMMANDS
#define LOAD_IDLE_MS 60000   /* abandon a paste that stops arriving */

/* CFG-6: read a JSON document from the console, validate it and (unless @p dry_run) save it
 * and restart. The document ends when its outer object closes; a line with a single '.'
 * aborts. */
static int config_load(bool dry_run)
{
    /* grows with the document (up to the limit): without PSRAM 128 KiB at once may not exist */
    const size_t max = CONFIG_GW_CONFIG_MAX_BYTES + 1;
    size_t cap = 8192;
    char *buf = heap_caps_malloc_prefer(cap, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                        MALLOC_CAP_8BIT);
    if (buf == NULL) {
        out("config: no memory\n");
        return 1;
    }
    gw_jsonfeed_t f;
    gw_jsonfeed_init(&f, buf, cap);
    out("paste the JSON document; it ends when its outer object closes ('.' alone aborts)\n");
    out_flush();
    char line[256];
    int64_t last = esp_timer_get_time();
    bool aborted = false;
    while (!f.complete && !aborted) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            clearerr(stdin);
            if (esp_timer_get_time() - last > LOAD_IDLE_MS * 1000LL) {
                aborted = true;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        last = esp_timer_get_time();
        aborted = strcmp(line, ".\n") == 0 || strcmp(line, ".\r\n") == 0 || strcmp(line, ".") == 0;
        size_t need = f.len + strlen(line) + 1;
        if (!aborted && need > f.cap && f.cap < max) {
            size_t ncap = 2 * f.cap > need ? 2 * f.cap : need;
            ncap = ncap < max ? ncap : max;
            char *nb = heap_caps_realloc_prefer(f.buf, ncap, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                                MALLOC_CAP_8BIT);
            if (nb != NULL) {
                f.buf = buf = nb;
                f.cap = ncap;
            }
        }
        if (!aborted) {
            gw_jsonfeed_add(&f, line);
        }
    }
    int rc = 1;
    gw_cfg_err_t err;
    if (aborted) {
        out("config: aborted, nothing changed\n");
    } else if (f.overflow) {
        out("config: rejected: larger than %d bytes (CONFIG_GW_CONFIG_MAX_BYTES) or no memory\n",
               CONFIG_GW_CONFIG_MAX_BYTES);
    } else if (gw_storage_save_config(buf, f.len, dry_run, &err) != ESP_OK) {
        out("config: rejected: %s\n", err.msg);
        if (err.path[0] != '\0') {
            out("config: path %s\n", err.path);
        } else if (err.line != 0) {
            out("config: line %u, column %u\n", err.line, err.column);
        }
    } else {
        out(dry_run ? "config: valid (%u bytes), nothing saved\n"
                       : "config: saved (%u bytes); restarting\n",
               (unsigned)f.len);
        rc = 0;
    }
    memset(buf, 0, f.len);   /* may hold secrets in clear */
    heap_caps_free(buf);
    if (rc == 0 && !dry_run) {
        out_flush();
        s_ctx.restart();
    }
    return rc;
}
#endif

static int cmd_config(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "show") == 0) {
        return config_show();
    }
#if CONFIG_GW_SHELL_WRITE_COMMANDS
    if (argc == 2 && (strcmp(argv[1], "load") == 0 || strcmp(argv[1], "check") == 0)) {
        return config_load(strcmp(argv[1], "check") == 0);
    }
#endif
    if (argc != 1) {
        out("usage: config [show%s]\n",
               CONFIG_GW_SHELL_WRITE_COMMANDS ? "|load|check" : "");
        return 1;
    }
    static const char *const parity[] = {"none", "even", "odd"};
    const gw_config_t *c = s_ctx.cfg;
    if (c == NULL) {
        out("REJECTED (%s): %s\n", s_ctx.config_source, s_ctx.config_error);
        return 1;
    }
    out("source     %s\n", s_ctx.config_source);
    out("sparkplug  group '%s', edge node '%s'\n", c->sparkplug.group_id,
           c->sparkplug.edge_node_id);
    out("mqtt       %s:%u, MQTT %s, client id '%s', keepalive %u s, user %s\n", c->mqtt.host,
           c->mqtt.port, c->mqtt.protocol == GW_MQTT_V5 ? "5" : "3.1.1", c->mqtt.client_id,
           c->mqtt.keepalive_s, c->mqtt.username[0] ? c->mqtt.username : "(none)");
    for (uint8_t i = 0; i < c->mqtt.backup_count; i++) {
        out("backup %u   %s:%u\n", i + 1u, c->mqtt.backup[i].host, c->mqtt.backup[i].port);
    }
    if (c->mqtt.backup_count > 0 && c->mqtt.primary_return_s > 0) {
        out("failover   after %u failed attempt(s); back to the primary after %lu s on a backup\n",
            c->mqtt.failover_after, (unsigned long)c->mqtt.primary_return_s);
    } else if (c->mqtt.backup_count > 0) {
        out("failover   after %u failed attempt(s); a working backup is kept\n",
            c->mqtt.failover_after);
    }
    const gw_tls_cfg_t *tls = &c->mqtt.tls;
    if (tls->enabled) {
        out("tls        CA %s%s, name checked against %s, client certificate %s%s\n",
            tls->ca_file[0] ? "/cfg/" : "ESP-IDF root bundle", tls->ca_file,
            tls->server_name[0] ? tls->server_name : "the broker host",
            tls->cert_file[0] ? "/cfg/" : "none", tls->cert_file);
    } else {
        out("tls        off (plain TCP)\n");
    }
    if (c->network.dhcp) {
        out("network    DHCP\n");
    } else {
        const uint8_t *a = c->network.ip, *m = c->network.netmask, *g = c->network.gateway;
        out("network    static %u.%u.%u.%u mask %u.%u.%u.%u gw %u.%u.%u.%u\n", a[0], a[1], a[2],
               a[3], m[0], m[1], m[2], m[3], g[0], g[1], g[2], g[3]);
    }
    const gw_wifi_cfg_t *w = &c->network.wifi;
    if (w->enabled) {
        out("wifi       '%s', %s, failback %u s, %s\n", w->ssid,
               w->role == GW_WIFI_PRIMARY ? "primary (Ethernet is the backup)" : "backup of Ethernet",
               w->failback_s, w->ipv4.dhcp ? "DHCP" : "static IPv4");
    } else {
        out("wifi       disabled\n");
    }
    const gw_ipv6_cfg_t *v6 = &c->network.ipv6;
    if (v6->enabled) {
        char a[48] = "none", d[48] = "none";
        ip6_addr_t t;
        if (v6->has_address) {
            memcpy(t.addr, v6->address, sizeof(t.addr));
            ip6addr_ntoa_r(&t, a, sizeof(a));
        }
        if (v6->has_dns) {
            memcpy(t.addr, v6->dns, sizeof(t.addr));
            ip6addr_ntoa_r(&t, d, sizeof(d));
        }
        for (char *ch = a; *ch != '\0'; ch++) {   /* RFC 5952: lower case */
            *ch = (char)tolower((unsigned char)*ch);
        }
        for (char *ch = d; *ch != '\0'; ch++) {
            *ch = (char)tolower((unsigned char)*ch);
        }
        out("ipv6       SLAAC %s, static address %s, DNS %s\n", v6->slaac ? "on" : "off", a, d);
    } else {
        out("ipv6       disabled\n");
    }
    out("ntp        %s\n", c->ntp.server);
    static const char *const levels[8] = {[3] = "error", [4] = "warning", [6] = "info",
                                          [7] = "debug"};
    if (c->syslog.host[0] != '\0') {
        out("syslog     %s:%u, level %s and above\n", c->syslog.host, c->syslog.port,
               levels[c->syslog.level & 7]);
    } else {
        out("syslog     disabled\n");
    }
    out("rs485      %lu baud, parity %s, %u stop bit(s), timeout %u ms, %u retries, "
           "inter-frame %u ms\n",
           (unsigned long)c->rs485.baud, parity[c->rs485.parity], c->rs485.stop_bits,
           c->rs485.response_timeout_ms, c->rs485.retries, c->rs485.inter_frame_delay_ms);
    out("local_io   DI debounce %u ms\n", c->local_io.di_debounce_ms);
    out("devices    %u device(s), %u tag(s)\n", c->device_count, c->tag_count);
    for (uint16_t d = 0; d < c->device_count; d++) {
        const gw_device_cfg_t *dev = &c->devices[d];
        out("  %-16s unit %3u, poll %lu ms, offline after %u, %u tag(s)\n", dev->name,
               dev->unit_id, (unsigned long)dev->poll_ms, dev->offline_after, dev->tag_count);
    }
    return 0;
}

/* ICMP echo to an IPv4 or IPv6 literal: reachability over the active interface. */
static SemaphoreHandle_t s_ping_done;

/* The ping task prints from its own task: one locked write per line. */
static void line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void line(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        write_locked(buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
    }
}

static void on_ping_reply(esp_ping_handle_t h, void *arg)
{
    (void)arg;
    uint16_t seq;
    uint32_t ms;
    ip_addr_t from;
    char text[48];
    esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &ms, sizeof(ms));
    esp_ping_get_profile(h, ESP_PING_PROF_IPADDR, &from, sizeof(from));
    line("reply from %s: seq %u time %lu ms\n", ipaddr_ntoa_r(&from, text, sizeof(text)), seq,
           (unsigned long)ms);
}

static void on_ping_timeout(esp_ping_handle_t h, void *arg)
{
    (void)arg;
    uint16_t seq;
    esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    line("seq %u: timeout\n", seq);
}

static void on_ping_end(esp_ping_handle_t h, void *arg)
{
    (void)arg;
    uint32_t sent, received;
    esp_ping_get_profile(h, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
    esp_ping_get_profile(h, ESP_PING_PROF_REPLY, &received, sizeof(received));
    line("%lu sent, %lu received\n", (unsigned long)sent, (unsigned long)received);
    xSemaphoreGive(s_ping_done);
}

static int cmd_ping(int argc, char **argv)
{
    esp_ping_config_t pc = ESP_PING_DEFAULT_CONFIG();
    if (argc != 2 || !ipaddr_aton(argv[1], &pc.target_addr)) {
        out("usage: ping <IPv4 or IPv6 address>\n");
        return 1;
    }
    pc.count = 3;
    if (s_ping_done == NULL) {
        s_ping_done = xSemaphoreCreateBinary();
    }
    const esp_ping_callbacks_t cbs = {
        .on_ping_success = on_ping_reply, .on_ping_timeout = on_ping_timeout, .on_ping_end = on_ping_end};
    esp_ping_handle_t h;
    if (s_ping_done == NULL || esp_ping_new_session(&pc, &cbs, &h) != ESP_OK) {
        out("ping: cannot start\n");
        return 1;
    }
    esp_ping_start(h);
    xSemaphoreTake(s_ping_done, pdMS_TO_TICKS(10000));
    esp_ping_stop(h);
    esp_ping_delete_session(h);
    return 0;
}

#if CONFIG_GW_SHELL_WRITE_COMMANDS
static int cmd_restart(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    out("restarting\n");
    out_flush();
    s_ctx.restart();
    return 0;
}

static int cmd_rebirth(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (s_ctx.cfg == NULL || !gw_edge_request_rebirth()) {
        out("rebirth: no Sparkplug session\n");
        return 1;
    }
    out("rebirth: NBIRTH and every DBIRTH requested\n");
    return 0;
}

static int cmd_mbread(int argc, char **argv)
{
    static const char *const areas[] = {"coil", "discrete", "holding", "input"};
    static const mb_fc_t fcs[] = {MB_FC_READ_COILS, MB_FC_READ_DISCRETE_INPUTS,
                                  MB_FC_READ_HOLDING_REGISTERS, MB_FC_READ_INPUT_REGISTERS};
    int area = -1;
    for (int i = 0; argc >= 4 && i < 4; i++) {
        area = strcmp(argv[2], areas[i]) == 0 ? i : area;
    }
    long unit = argc >= 4 ? strtol(argv[1], NULL, 10) : 0;
    long addr = argc >= 4 ? strtol(argv[3], NULL, 10) : -1;
    long count = argc == 5 ? strtol(argv[4], NULL, 10) : 1;
    if (argc < 4 || argc > 5 || area < 0 || unit < 1 || unit > 247 || addr < 0 ||
        addr > 65535 || count < 1 || count > GW_BUS_DIAG_MAX || addr + count > 65536) {
        out("usage: mbread <unit 1-247> <coil|discrete|holding|input> <address> [count 1-%d]\n",
               GW_BUS_DIAG_MAX);
        return 1;
    }
    if (s_ctx.cfg == NULL) {
        out("mbread: the bus is not running (no valid configuration)\n");
        return 1;
    }
    uint16_t v[GW_BUS_DIAG_MAX];
    uint8_t exc;
    /* worst case: a poll cycle in progress plus every retry of this request */
    uint32_t wait = 2000u + (uint32_t)s_ctx.cfg->rs485.response_timeout_ms *
                                (s_ctx.cfg->rs485.retries + 1u) * 2u;
    mb_status_t st = gw_bus_diag_read((uint8_t)unit, fcs[area], (uint16_t)addr, (uint16_t)count,
                                      v, &exc, wait);
    static const char *const status[] = {"ok", "timeout", "CRC error", "framing error",
                                         "exception", "busy or bus not answering in time"};
    if (st != MB_OK) {
        if (st == MB_ERR_EXCEPTION) {
            out("mbread: unit %ld %s %ld: exception %u\n", unit, areas[area], addr, exc);
        } else {
            out("mbread: unit %ld %s %ld: %s\n", unit, areas[area], addr,
                   status[st <= MB_ERR_ARG ? st : MB_ERR_ARG]);
        }
        return 1;
    }
    out("mbread: unit %ld %s %ld:", unit, areas[area], addr);
    for (long i = 0; i < count; i++) {
        out(" %u", v[i]);
    }
    out("\n");
    return 0;
}
#endif

#if CONFIG_GW_IO_SIMULATED
static int cmd_di(int argc, char **argv)
{
    long ch = argc == 3 ? strtol(argv[1], NULL, 10) : 0;
    long level = argc == 3 ? strtol(argv[2], NULL, 10) : -1;
    if (ch < 1 || ch > 8 || (level != 0 && level != 1)) {
        out("usage: di <1-8> <0|1>\n");
        return 1;
    }
    gw_io_sim_set_di((uint8_t)(ch - 1), level == 1);
    return 0;
}

static int cmd_boot(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "0") != 0 && strcmp(argv[1], "1") != 0)) {
        out("usage: boot <1 = press | 0 = release>\n");
        return 1;
    }
    gw_bootbtn_sim_set(argv[1][0] == '1');
    return 0;
}
/* Emulator only: crash on purpose, so the E2E can check the core dump in flash and the
 * "panic" reset reason. */
static int cmd_panic(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("panic requested from the console\n");
    fflush(stdout);
    abort();
}
#endif

ATOMIC(cmd_status)
ATOMIC(cmd_devices)
ATOMIC(cmd_config)
ATOMIC(cmd_ping)
#if CONFIG_GW_SHELL_WRITE_COMMANDS
ATOMIC(cmd_restart)
ATOMIC(cmd_rebirth)
ATOMIC(cmd_mbread)
#endif
#if CONFIG_GW_IO_SIMULATED
ATOMIC(cmd_di)
ATOMIC(cmd_boot)
#endif

static esp_err_t new_repl(const esp_console_repl_config_t *rc, esp_console_repl_t **repl)
{
#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    const esp_console_dev_uart_config_t dev = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    return esp_console_new_repl_uart(&dev, rc, repl);
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    const esp_console_dev_usb_serial_jtag_config_t dev =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    return esp_console_new_repl_usb_serial_jtag(&dev, rc, repl);
#else
    (void)rc;
    (void)repl;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t gw_shell_start(const gw_shell_ctx_t *ctx)
{
    s_ctx = *ctx;
    s_console = xSemaphoreCreateRecursiveMutexStatic(&s_console_buf);
    s_prev_vprintf = esp_log_set_vprintf(log_vprintf);
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "gw>";
    esp_console_repl_t *repl;
    ESP_RETURN_ON_ERROR(new_repl(&rc, &repl), TAG, "console REPL");

    static const esp_console_cmd_t cmds[] = {
        {.command = "status", .help = "Gateway, configuration, network, MQTT and I/O state",
         .func = cmd_status_atomic},
        {.command = "devices", .help = "Modbus devices: online state and counters",
         .func = cmd_devices_atomic},
        {.command = "config",
         .help = "Summary of the loaded configuration; 'show' prints the stored document "
                 "(secrets masked)"
#if CONFIG_GW_SHELL_WRITE_COMMANDS
                 ", 'load' reads a new one from the console, validates, saves and restarts, "
                 "'check' only validates"
#endif
         ,
         .hint = "[show|load|check]", .func = cmd_config_atomic},
#if CONFIG_GW_SHELL_WRITE_COMMANDS
        {.command = "restart", .help = "Orderly restart (NDEATH first)", .func = cmd_restart_atomic},
        {.command = "rebirth", .help = "Re-issue NBIRTH and every DBIRTH", .func = cmd_rebirth_atomic},
        {.command = "mbread", .help = "One Modbus read through the bus owner",
         .hint = "<unit> <coil|discrete|holding|input> <address> [count]", .func = cmd_mbread_atomic},
#endif
        {.command = "ping", .help = "ICMP echo (3 requests) to an IPv4 or IPv6 address",
         .hint = "<address>", .func = cmd_ping_atomic},
#if CONFIG_GW_IO_SIMULATED
        {.command = "di", .help = "Emulator only: set a simulated input", .hint = "<1-8> <0|1>",
         .func = cmd_di_atomic},
        {.command = "boot", .help = "Emulator only: press (1) or release (0) the BOOT button",
         .hint = "<0|1>", .func = cmd_boot_atomic},
        {.command = "panic", .help = "Emulator only: abort() (core dump test)", .func = cmd_panic},
#endif
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmds[i]), TAG, "%s", cmds[i].command);
    }
    ESP_RETURN_ON_ERROR(esp_console_register_help_command(), TAG, "help");
    return esp_console_start_repl(repl);
}
