#include "gw_shell.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gw_io.h"
#include "gw_log.h"
#include "gw_net.h"
#include "gw_status.h"
#include "gw_tagdb.h"
#include "lwip/ip_addr.h"
#include "ping/ping_sock.h"

static const char *TAG = "shell";

static gw_shell_ctx_t s_ctx;
static gw_tag_value_t s_vals[GW_MAX_TAGS_PER_DEVICE];   /* scratch for gw_tagdb_get_device */

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
    printf("firmware %s, reset reason %s, uptime %lld s\n", s_ctx.info->firmware,
           s_ctx.info->reset_reason, esp_timer_get_time() / 1000000);
    if (s_ctx.cfg == NULL) {
        printf("config   REJECTED (%s): %s\n", s_ctx.config_source, s_ctx.config_error);
        printf("         nothing is running: no polling, no network, no MQTT\n");
        return 0;
    }
    uint16_t offline = 0;
    for (uint16_t d = 0; d < s_ctx.cfg->device_count; d++) {
        offline = (uint16_t)(offline + (device_online(d) ? 0u : 1u));
    }
    printf("config   %s: ok\n", s_ctx.config_source);
    printf("network  %s, MQTT %s, Modbus %u/%u devices online\n",
           (f & GW_ST_NET_UP) ? "up" : "down", (f & GW_ST_MQTT_UP) ? "connected" : "disconnected",
           s_ctx.cfg->device_count - offline, s_ctx.cfg->device_count);
    gw_net_if_status_t ifs[GW_NET_IF_COUNT];
    gw_net_get_status(ifs);
    static const char *const names[GW_NET_IF_COUNT] = {"ethernet", "wifi"};
    printf("iface    ");
    for (int i = 0; i < GW_NET_IF_COUNT; i++) {
        const gw_net_if_status_t *n = &ifs[i];
        printf("%s%s", i ? " | " : "", names[i]);
        if (!n->configured) {
            printf(" not configured");
            continue;
        }
        printf(" %s", !n->link ? "link down" : n->usable ? "up" : "link up, no address");
        if (n->usable) {
            printf(" %u.%u.%u.%u", n->ipv4[0], n->ipv4[1], n->ipv4[2], n->ipv4[3]);
        }
        if (n->active) {
            printf(" (active)");
        }
    }
    printf("\n");
    bool any6 = false;
    for (int i = 0; i < GW_NET_IF_COUNT; i++) {
        for (int k = 0; k < GW_NET_IPV6_MAX; k++) {
            if (ifs[i].ipv6[k][0] != '\0') {
                printf("%s%s %s", any6 ? ", " : "ipv6     ", names[i], ifs[i].ipv6[k]);
                any6 = true;
            }
        }
    }
    if (any6) {
        printf("\n");
    }
    gw_log_stats_t ls;
    gw_log_get_stats(&ls);
    if (ls.enabled) {
        printf("syslog   %s:%u %s, sent %lu, dropped %lu, errors %lu\n", s_ctx.cfg->syslog.host,
               s_ctx.cfg->syslog.port, ls.connected ? "connected" : "not reachable",
               (unsigned long)ls.sent, (unsigned long)ls.dropped, (unsigned long)ls.send_errors);
    }
    gw_local_io_t io;
    gw_tagdb_get_io(&io);
    printf("local    DI 0x%02x, DO 0x%02x (bit n = channel n+1, 1 = active)\n", io.di, io.dout);
    printf("heap     internal %u B free (minimum %u B), PSRAM %u B free\n",
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
        printf("no configuration\n");
        return 1;
    }
    for (uint16_t d = 0; d < s_ctx.cfg->device_count; d++) {
        const gw_device_cfg_t *dev = &s_ctx.cfg->devices[d];
        gw_dev_stats_t st;
        gw_tagdb_get_stats(d, &st);
        printf("%-16s unit %3u %-7s cycles %lu failed %lu | ok %lu timeout %lu crc %lu frame %lu "
               "exception %lu\n",
               dev->name, dev->unit_id, device_online(d) ? "online" : "offline",
               (unsigned long)st.cycles, (unsigned long)st.failed_cycles, (unsigned long)st.bus.ok,
               (unsigned long)st.bus.timeout, (unsigned long)st.bus.crc,
               (unsigned long)st.bus.frame, (unsigned long)st.bus.exception);
    }
    return 0;
}

static int cmd_config(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    static const char *const parity[] = {"none", "even", "odd"};
    const gw_config_t *c = s_ctx.cfg;
    if (c == NULL) {
        printf("REJECTED (%s): %s\n", s_ctx.config_source, s_ctx.config_error);
        return 1;
    }
    printf("source     %s\n", s_ctx.config_source);
    printf("sparkplug  group '%s', edge node '%s'\n", c->sparkplug.group_id,
           c->sparkplug.edge_node_id);
    printf("mqtt       %s:%u, client id '%s', keepalive %u s, user %s\n", c->mqtt.host,
           c->mqtt.port, c->mqtt.client_id, c->mqtt.keepalive_s,
           c->mqtt.username[0] ? c->mqtt.username : "(none)");
    if (c->network.dhcp) {
        printf("network    DHCP\n");
    } else {
        const uint8_t *a = c->network.ip, *m = c->network.netmask, *g = c->network.gateway;
        printf("network    static %u.%u.%u.%u mask %u.%u.%u.%u gw %u.%u.%u.%u\n", a[0], a[1], a[2],
               a[3], m[0], m[1], m[2], m[3], g[0], g[1], g[2], g[3]);
    }
    const gw_wifi_cfg_t *w = &c->network.wifi;
    if (w->enabled) {
        printf("wifi       '%s', %s, failback %u s, %s\n", w->ssid,
               w->role == GW_WIFI_PRIMARY ? "primary (Ethernet is the backup)" : "backup of Ethernet",
               w->failback_s, w->ipv4.dhcp ? "DHCP" : "static IPv4");
    } else {
        printf("wifi       disabled\n");
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
        printf("ipv6       SLAAC %s, static address %s, DNS %s\n", v6->slaac ? "on" : "off", a, d);
    } else {
        printf("ipv6       disabled\n");
    }
    printf("ntp        %s\n", c->ntp.server);
    static const char *const levels[8] = {[3] = "error", [4] = "warning", [6] = "info",
                                          [7] = "debug"};
    if (c->syslog.host[0] != '\0') {
        printf("syslog     %s:%u, level %s and above\n", c->syslog.host, c->syslog.port,
               levels[c->syslog.level & 7]);
    } else {
        printf("syslog     disabled\n");
    }
    printf("rs485      %lu baud, parity %s, %u stop bit(s), timeout %u ms, %u retries, "
           "inter-frame %u ms\n",
           (unsigned long)c->rs485.baud, parity[c->rs485.parity], c->rs485.stop_bits,
           c->rs485.response_timeout_ms, c->rs485.retries, c->rs485.inter_frame_delay_ms);
    printf("local_io   DI debounce %u ms\n", c->local_io.di_debounce_ms);
    printf("devices    %u device(s), %u tag(s)\n", c->device_count, c->tag_count);
    for (uint16_t d = 0; d < c->device_count; d++) {
        const gw_device_cfg_t *dev = &c->devices[d];
        printf("  %-16s unit %3u, poll %lu ms, offline after %u, %u tag(s)\n", dev->name,
               dev->unit_id, (unsigned long)dev->poll_ms, dev->offline_after, dev->tag_count);
    }
    return 0;
}

/* ICMP echo to an IPv4 or IPv6 literal: reachability over the active interface. */
static SemaphoreHandle_t s_ping_done;

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
    printf("reply from %s: seq %u time %lu ms\n", ipaddr_ntoa_r(&from, text, sizeof(text)), seq,
           (unsigned long)ms);
}

static void on_ping_timeout(esp_ping_handle_t h, void *arg)
{
    (void)arg;
    uint16_t seq;
    esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    printf("seq %u: timeout\n", seq);
}

static void on_ping_end(esp_ping_handle_t h, void *arg)
{
    (void)arg;
    uint32_t sent, received;
    esp_ping_get_profile(h, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
    esp_ping_get_profile(h, ESP_PING_PROF_REPLY, &received, sizeof(received));
    printf("%lu sent, %lu received\n", (unsigned long)sent, (unsigned long)received);
    xSemaphoreGive(s_ping_done);
}

static int cmd_ping(int argc, char **argv)
{
    esp_ping_config_t pc = ESP_PING_DEFAULT_CONFIG();
    if (argc != 2 || !ipaddr_aton(argv[1], &pc.target_addr)) {
        printf("usage: ping <IPv4 or IPv6 address>\n");
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
        printf("ping: cannot start\n");
        return 1;
    }
    esp_ping_start(h);
    xSemaphoreTake(s_ping_done, pdMS_TO_TICKS(10000));
    esp_ping_stop(h);
    esp_ping_delete_session(h);
    return 0;
}

#if CONFIG_GW_IO_SIMULATED
static int cmd_di(int argc, char **argv)
{
    long ch = argc == 3 ? strtol(argv[1], NULL, 10) : 0;
    long level = argc == 3 ? strtol(argv[2], NULL, 10) : -1;
    if (ch < 1 || ch > 8 || (level != 0 && level != 1)) {
        printf("usage: di <1-8> <0|1>\n");
        return 1;
    }
    gw_io_sim_set_di((uint8_t)(ch - 1), level == 1);
    return 0;
}
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
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "gw>";
    esp_console_repl_t *repl;
    ESP_RETURN_ON_ERROR(new_repl(&rc, &repl), TAG, "console REPL");

    static const esp_console_cmd_t cmds[] = {
        {.command = "status", .help = "Gateway, configuration, network, MQTT and I/O state",
         .func = cmd_status},
        {.command = "devices", .help = "Modbus devices: online state and counters",
         .func = cmd_devices},
        {.command = "config", .help = "Summary of the loaded configuration", .func = cmd_config},
        {.command = "ping", .help = "ICMP echo (3 requests) to an IPv4 or IPv6 address",
         .hint = "<address>", .func = cmd_ping},
#if CONFIG_GW_IO_SIMULATED
        {.command = "di", .help = "Emulator only: set a simulated input", .hint = "<1-8> <0|1>",
         .func = cmd_di},
#endif
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmds[i]), TAG, "%s", cmds[i].command);
    }
    ESP_RETURN_ON_ERROR(esp_console_register_help_command(), TAG, "help");
    return esp_console_start_repl(repl);
}
