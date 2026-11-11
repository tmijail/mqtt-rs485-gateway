#include "gw_net.h"

#include <ctype.h>
#include <string.h>

#include "esp_check.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "gw_backoff.h"
#include "gw_netsel.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip6_addr.h"

#if CONFIG_GW_ETH_W5500
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "gw_bsp.h"
#endif

#if CONFIG_GW_WIFI
#include "esp_wifi.h"
#endif

static const char *TAG = "net";

/* Interface ranks for gw_netsel; esp_netif route priorities follow the same order so the
 * automatic default-route choice of esp_netif never contradicts it (ETH default: 50). */
#define ROUTE_PRIO_ETH          50
#define ROUTE_PRIO_WIFI_BACKUP  20
#define ROUTE_PRIO_WIFI_PRIMARY 100
#define WIFI_RETRY_MIN_MS       1000
#define WIFI_RETRY_MAX_MS       30000

ESP_EVENT_DEFINE_BASE(GW_NET_EVENT);
enum { GW_NET_EV_POLL, GW_NET_EV_UNREACHABLE, GW_NET_EV_REACHABLE };

typedef struct {
    esp_netif_t *netif;
    const char *name;
    bool link;
    bool v4;
    bool v6;                    /* a global or unique-local IPv6 address (NET-7) */
    esp_ip4_addr_t ip;
    char ip6[GW_NET_IPV6_MAX][GW_NET_IPV6_STR];
} iface_t;

/* ---- owned by the default event loop task ---- */
static const gw_config_t *s_cfg;
static gw_net_callbacks_t s_cb;
static iface_t s_if[GW_NET_IF_COUNT] = {[GW_NET_ETH] = {.name = "ethernet"},
                                        [GW_NET_WIFI] = {.name = "wifi"}};
static uint8_t s_if_count;          /* 1 = Ethernet only (the MVP), 2 = Ethernet + Wi-Fi */
static gw_netsel_t s_sel;
static int s_session_if = GW_NETSEL_NONE;   /* last active interface (the MQTT binding) */
static esp_timer_handle_t s_poll_timer;
static bool s_sntp_started;

/* ---- read by other tasks ---- */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static gw_net_if_status_t s_status[GW_NET_IF_COUNT];
static char s_active_name[8];

static esp_ip4_addr_t ip4(const uint8_t a[4])
{
    esp_ip4_addr_t r;
    IP4_ADDR(&r, a[0], a[1], a[2], a[3]);
    return r;
}

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static void publish_status(void)
{
    char name[8] = "";
    if (s_if_count > 1 && s_sel.active != GW_NETSEL_NONE) {
        esp_netif_get_netif_impl_name(s_if[s_sel.active].netif, name);
    }
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < GW_NET_IF_COUNT; i++) {
        gw_net_if_status_t *st = &s_status[i];
        st->configured = i < s_if_count;
        st->link = s_if[i].link;
        st->usable = s_sel.usable[i];
        st->active = s_sel.active == i;
        memcpy(st->ipv4, &s_if[i].ip.addr, 4);
        memcpy(st->ipv6, s_if[i].ip6, sizeof(st->ipv6));
    }
    memcpy(s_active_name, name, sizeof(s_active_name));
    portEXIT_CRITICAL(&s_mux);
}

bool gw_net_active_ifname(char *name, size_t cap)
{
    if (cap == 0) {
        return false;
    }
    portENTER_CRITICAL(&s_mux);
    size_t n = strnlen(s_active_name, sizeof(s_active_name));
    bool fits = n < cap;
    memcpy(name, s_active_name, fits ? n : 0);
    portEXIT_CRITICAL(&s_mux);
    name[fits ? n : 0] = '\0';
    return fits && n > 0;
}

bool gw_net_has_wifi(void)
{
#if CONFIG_GW_WIFI
    gw_net_if_status_t st[GW_NET_IF_COUNT];
    gw_net_get_status(st);
    return st[GW_NET_WIFI].configured;
#else
    return false;
#endif
}

bool gw_net_wifi_rssi(int8_t *rssi)
{
#if CONFIG_GW_WIFI
    gw_net_if_status_t st[GW_NET_IF_COUNT];
    gw_net_get_status(st);
    wifi_ap_record_t ap;
    if (!st[GW_NET_WIFI].configured || !st[GW_NET_WIFI].link ||
        esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return false;
    }
    *rssi = ap.rssi;
    return true;
#else
    (void)rssi;
    return false;
#endif
}

void gw_net_get_status(gw_net_if_status_t out[GW_NET_IF_COUNT])
{
    portENTER_CRITICAL(&s_mux);
    memcpy(out, s_status, sizeof(s_status));
    portEXIT_CRITICAL(&s_mux);
}

/* Apply a decision of gw_netsel: default route, callbacks, failback timer. */
static void apply(gw_netsel_change_t c)
{
    if (c.event != GW_NETSEL_UNCHANGED) {
        const char *from = c.from == GW_NETSEL_NONE ? "none" : s_if[c.from].name;
        const char *to = c.to == GW_NETSEL_NONE ? "none" : s_if[c.to].name;
        if (c.event == GW_NETSEL_LOST) {
            ESP_LOGW(TAG, "active interface: %s -> none", from);
        } else {
            ESP_LOGI(TAG, "active interface: %s -> %s", from, to);
        }
        if (c.to != GW_NETSEL_NONE && s_if_count > 1) {
            esp_netif_set_default_netif(s_if[c.to].netif);
        }
        publish_status();
        if (c.to != GW_NETSEL_NONE) {
            /* A TCP connection cannot move between interfaces: whenever the session would now
             * run over another interface than before, it is re-established over the new one. */
            if (s_session_if != GW_NETSEL_NONE && c.to != s_session_if) {
                s_cb.route_changed();
            }
            s_session_if = c.to;
        }
        if (c.event == GW_NETSEL_ACTIVATED) {
            s_cb.ip_changed(true);
        } else if (c.event == GW_NETSEL_LOST) {
            s_cb.ip_changed(false);
        }
    }
    if (s_poll_timer != NULL) {
        uint32_t ms = gw_netsel_next_poll_ms(&s_sel, now_ms());
        esp_timer_stop(s_poll_timer);
        if (ms != UINT32_MAX) {
            esp_timer_start_once(s_poll_timer, ((uint64_t)ms + 1) * 1000);
        }
    }
}

static void update(gw_net_if_t i)
{
    iface_t *f = &s_if[i];
    apply(gw_netsel_set(&s_sel, (uint8_t)i, f->link && (f->v4 || f->v6), now_ms()));
    publish_status();
}

static void on_poll_timer(void *arg)
{
    (void)arg;
    /* runs in the esp_timer task: hand over to the event loop, the owner of the state */
    esp_event_post(GW_NET_EVENT, GW_NET_EV_POLL, NULL, 0, 0);
}

static void on_gw_net_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == GW_NET_EV_POLL) {
        apply(gw_netsel_poll(&s_sel, now_ms()));
        return;
    }
    int i = *(const int *)data;
    if (id == GW_NET_EV_UNREACHABLE) {
        uint32_t hold = s_sel.hold_ms[i];
        ESP_LOGW(TAG, "%s: no broker reachable over it (link and address are up): without service "
                      "for %lu s", s_if[i].name, (unsigned long)(hold / 1000));
        apply(gw_netsel_unreachable(&s_sel, (uint8_t)i, now_ms()));
    } else if (id == GW_NET_EV_REACHABLE) {
        if (s_sel.held_until_ms[i] != 0 || s_sel.hold_ms[i] != s_sel.hold_min_ms) {
            ESP_LOGI(TAG, "%s: the broker is reachable again", s_if[i].name);
        }
        gw_netsel_reachable(&s_sel, (uint8_t)i);
        apply(gw_netsel_poll(&s_sel, now_ms()));
    }
}

void gw_net_report_broker(const char *ifname, bool reachable)
{
    if (s_if_count < 2 || ifname == NULL || ifname[0] == '\0') {
        return;   /* one interface: nowhere to move the session to */
    }
    for (int i = 0; i < s_if_count; i++) {
        char name[8] = "";
        esp_netif_get_netif_impl_name(s_if[i].netif, name);
        if (strcmp(name, ifname) == 0) {
            /* hand over to the event loop, the owner of the selection */
            esp_event_post(GW_NET_EVENT, reachable ? GW_NET_EV_REACHABLE : GW_NET_EV_UNREACHABLE,
                           &i, sizeof(i), 0);
            return;
        }
    }
}

/* NET-7: link-local address (and with it router solicitation and SLAAC) on every link-up,
 * plus the static address on Ethernet. lwIP keeps addresses across a link flap; adding an
 * existing one again is harmless. */
static void start_ipv6(gw_net_if_t i)
{
    const gw_ipv6_cfg_t *v6 = &s_cfg->network.ipv6;
    if (!v6->enabled) {
        return;
    }
    esp_err_t err = esp_netif_create_ip6_linklocal(s_if[i].netif);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: IPv6 link-local: %s", s_if[i].name, esp_err_to_name(err));
    }
    if (i == GW_NET_ETH && v6->has_address) {
        esp_ip6_addr_t a = {0};
        memcpy(a.addr, v6->address, sizeof(a.addr));
        err = esp_netif_add_ip6_address(s_if[i].netif, a, true);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "%s: static IPv6 address: %s", s_if[i].name, esp_err_to_name(err));
        }
    }
}

static void link_changed(gw_net_if_t i, bool up)
{
    s_if[i].link = up;
    if (!up) {
        /* a new address is needed after the link comes back (DHCP or the static one) */
        s_if[i].v4 = false;
        s_if[i].v6 = false;
        s_if[i].ip.addr = 0;
        memset(s_if[i].ip6, 0, sizeof(s_if[i].ip6));
    } else {
        start_ipv6(i);
    }
    update(i);
}

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == ETHERNET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "link up");
        link_changed(GW_NET_ETH, true);
    } else if (id == ETHERNET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "link down");
        link_changed(GW_NET_ETH, false);
    }
}

static void on_time_sync(struct timeval *tv)
{
    (void)tv;
    s_cb.time_synced();
}

static void got_ipv4(gw_net_if_t i, const ip_event_got_ip_t *ev)
{
    ESP_LOGI(TAG, "%sIPv4 " IPSTR " mask " IPSTR " gw " IPSTR, i == GW_NET_WIFI ? "wifi: " : "",
             IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.netmask), IP2STR(&ev->ip_info.gw));
    if (!s_sntp_started) {
        s_sntp_started = esp_netif_sntp_start() == ESP_OK;
    }
    s_if[i].v4 = true;
    s_if[i].ip = ev->ip_info.ip;
    update(i);
}

static void lost_ipv4(gw_net_if_t i)
{
    ESP_LOGW(TAG, "%sIPv4 lost", i == GW_NET_WIFI ? "wifi: " : "");
    s_if[i].v4 = false;
    s_if[i].ip.addr = 0;
    update(i);
}

static void got_ipv6(const ip_event_got_ip6_t *ev)
{
    int i = ev->esp_netif == s_if[GW_NET_ETH].netif    ? GW_NET_ETH
            : ev->esp_netif == s_if[GW_NET_WIFI].netif ? GW_NET_WIFI
                                                       : -1;
    if (i < 0) {
        return;
    }
    char text[GW_NET_IPV6_STR];
    ip6addr_ntoa_r((const ip6_addr_t *)&ev->ip6_info.ip, text, sizeof(text));
    for (char *c = text; *c != '\0'; c++) {   /* RFC 5952: lower case */
        *c = (char)tolower((unsigned char)*c);
    }
    esp_ip6_addr_type_t type = esp_netif_ip6_get_addr_type((esp_ip6_addr_t *)&ev->ip6_info.ip);
    const char *kind = type == ESP_IP6_ADDR_IS_LINK_LOCAL     ? "link-local"
                       : type == ESP_IP6_ADDR_IS_GLOBAL       ? "global"
                       : type == ESP_IP6_ADDR_IS_UNIQUE_LOCAL ? "unique local"
                       : type == ESP_IP6_ADDR_IS_SITE_LOCAL   ? "site local"
                                                              : "other";
    ESP_LOGI(TAG, "%s: IPv6 %s (%s)", s_if[i].name, text, kind);
    if (ev->ip_index >= 0 && ev->ip_index < GW_NET_IPV6_MAX) {
        memcpy(s_if[i].ip6[ev->ip_index], text, sizeof(text));
    }
    if (type != ESP_IP6_ADDR_IS_LINK_LOCAL && !s_if[i].v6) {
        if (!s_sntp_started) {
            s_sntp_started = esp_netif_sntp_start() == ESP_OK;
        }
        s_if[i].v6 = true;
    }
    update((gw_net_if_t)i);
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    switch (id) {
    case IP_EVENT_GOT_IP6:
        got_ipv6(data);
        break;
    case IP_EVENT_ETH_GOT_IP:
        got_ipv4(GW_NET_ETH, data);
        break;
    case IP_EVENT_ETH_LOST_IP:
        lost_ipv4(GW_NET_ETH);
        break;
    case IP_EVENT_STA_GOT_IP:
        got_ipv4(GW_NET_WIFI, data);
        break;
    case IP_EVENT_STA_LOST_IP:
        lost_ipv4(GW_NET_WIFI);
        break;
    default:
        break;
    }
}

static esp_err_t configure_ipv4(esp_netif_t *netif, const char *name, bool dhcp,
                                const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4],
                                const uint8_t dns[4])
{
    if (dhcp) {
        return ESP_OK;
    }
    esp_err_t err = esp_netif_dhcpc_stop(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return err;
    }
    const esp_netif_ip_info_t info = {.ip = ip4(ip), .netmask = ip4(mask), .gw = ip4(gw)};
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(netif, &info), TAG, "static ip");
    if (dns[0] | dns[1] | dns[2] | dns[3]) {
        esp_netif_dns_info_t d = {0};
        d.ip.type = ESP_IPADDR_TYPE_V4;
        d.ip.u_addr.ip4 = ip4(dns);
        ESP_RETURN_ON_ERROR(esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &d), TAG, "dns");
    }
    ESP_LOGI(TAG, "%s: static IPv4 %u.%u.%u.%u", name, ip[0], ip[1], ip[2], ip[3]);
    return ESP_OK;
}

/* SLAAC per interface (CONFIG_LWIP_IPV6_AUTOCONFIG compiles it in; the flag turns it on). */
static esp_netif_flags_t ipv6_flags(esp_netif_flags_t flags, const gw_ipv6_cfg_t *v6)
{
    if (v6->enabled && v6->slaac) {
        return (esp_netif_flags_t)(flags | ESP_NETIF_FLAG_IPV6_AUTOCONFIG_ENABLED);
    }
    return (esp_netif_flags_t)(flags & ~ESP_NETIF_FLAG_IPV6_AUTOCONFIG_ENABLED);
}

/* IPv6 DNS server in the third slot: DHCPv4 only fills the first two. */
static esp_err_t configure_ipv6_dns(esp_netif_t *netif, const gw_ipv6_cfg_t *v6)
{
    if (!v6->enabled || !v6->has_dns) {
        return ESP_OK;
    }
    esp_netif_dns_info_t d = {0};
    d.ip.type = ESP_IPADDR_TYPE_V6;
    memcpy(d.ip.u_addr.ip6.addr, v6->dns, sizeof(d.ip.u_addr.ip6.addr));
    return esp_netif_set_dns_info(netif, ESP_NETIF_DNS_FALLBACK, &d);
}

/* ---------------------------------------------------------------- Ethernet */

#if CONFIG_GW_IO_SIMULATED
/* Issue #144, emulator only: the driver, for gw_net_sim_stop_eth() */
static esp_eth_handle_t s_sim_eth;
#endif

static esp_err_t new_eth_driver(esp_eth_handle_t *eth)
{
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    /* Pin the RX task to the installing core: app_main runs on core 0, with the rest of the
     * network stack (sdkconfig.defaults), away from the RS485 bus on core 1. */
    mac_cfg.flags |= ETH_MAC_FLAG_PIN_TO_CORE;
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
#if CONFIG_GW_ETH_W5500
    ESP_RETURN_ON_ERROR(gpio_install_isr_service(0), TAG, "isr service");
    const spi_bus_config_t bus = {
        .mosi_io_num = gw_bsp.w5500.mosi,
        .miso_io_num = gw_bsp.w5500.miso,
        .sclk_io_num = gw_bsp.w5500.sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(gw_bsp.w5500.host, &bus, SPI_DMA_CH_AUTO), TAG,
                        "spi bus");
    spi_device_interface_config_t dev = {
        .mode = 0,
        .clock_speed_hz = (int)gw_bsp.w5500.clk_hz,
        .spics_io_num = gw_bsp.w5500.cs,
        .queue_size = 20,
    };
    eth_w5500_config_t w5500 = ETH_W5500_DEFAULT_CONFIG(gw_bsp.w5500.host, &dev);
    w5500.base.int_gpio_num = gw_bsp.w5500.irq;
    phy_cfg.reset_gpio_num = gw_bsp.w5500.rst;
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);
#else
    phy_cfg.reset_gpio_num = -1;
    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_cfg);
#endif
    ESP_RETURN_ON_FALSE(mac && phy, ESP_FAIL, TAG, "MAC/PHY creation failed");
    esp_eth_config_t ec = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&ec, eth), TAG, "eth driver");
#if CONFIG_GW_ETH_W5500
    /* The W5500 has no burned-in MAC: use the ESP32-S3's Ethernet MAC from eFuse. */
    uint8_t addr[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(addr, ESP_MAC_ETH), TAG, "mac");
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(*eth, ETH_CMD_S_MAC_ADDR, addr), TAG, "set mac");
#endif
    return ESP_OK;
}

static esp_err_t start_ethernet(const gw_config_t *cfg)
{
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.route_prio = ROUTE_PRIO_ETH;
    base.flags = ipv6_flags(base.flags, &cfg->network.ipv6);
    const esp_netif_config_t nc = {.base = &base, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH};
    s_if[GW_NET_ETH].netif = esp_netif_new(&nc);
    esp_netif_t *netif = s_if[GW_NET_ETH].netif;
    ESP_RETURN_ON_FALSE(netif, ESP_FAIL, TAG, "netif");
    /* NET-6: sent in DHCP requests (before the interface starts) */
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(netif, cfg->network.hostname), TAG, "hostname");
    ESP_RETURN_ON_ERROR(configure_ipv4(netif, "ethernet", cfg->network.dhcp, cfg->network.ip,
                                       cfg->network.netmask, cfg->network.gateway,
                                       cfg->network.dns),
                        TAG, "ip config");
    ESP_RETURN_ON_ERROR(configure_ipv6_dns(netif, &cfg->network.ipv6), TAG, "ipv6 dns");
    esp_eth_handle_t eth;
    ESP_RETURN_ON_ERROR(new_eth_driver(&eth), TAG, "driver");
#if CONFIG_GW_IO_SIMULATED
    s_sim_eth = eth;
#endif
    ESP_RETURN_ON_ERROR(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)), TAG, "attach");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL),
                        TAG, "eth events");
    return esp_eth_start(eth);
}

#if CONFIG_GW_IO_SIMULATED
void gw_net_sim_stop_eth(void)
{
    if (s_sim_eth != NULL) {
        esp_eth_stop(s_sim_eth);
    }
}
#endif

/* ---------------------------------------------------------------- Wi-Fi (NET-4) */

#if CONFIG_GW_WIFI
static esp_timer_handle_t s_wifi_retry_timer;
static uint32_t s_wifi_attempt;

static void wifi_connect(void *arg)
{
    (void)arg;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi: esp_wifi_connect: %s", esp_err_to_name(err));
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    switch (id) {
    case WIFI_EVENT_STA_START:
        wifi_connect(NULL);
        break;
    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "wifi: associated with '%s'", s_cfg->network.wifi.ssid);
        s_wifi_attempt = 0;
        link_changed(GW_NET_WIFI, true);
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *ev = data;
        uint32_t delay = gw_backoff_ms(++s_wifi_attempt, WIFI_RETRY_MIN_MS, WIFI_RETRY_MAX_MS,
                                       esp_random());
        ESP_LOGW(TAG, "wifi: disconnected from '%s' (reason %u, rssi %d); retry %lu in %lu ms",
                 s_cfg->network.wifi.ssid, ev->reason, ev->rssi, (unsigned long)s_wifi_attempt,
                 (unsigned long)delay);
        if (s_if[GW_NET_WIFI].link) {
            link_changed(GW_NET_WIFI, false);
        }
        esp_timer_stop(s_wifi_retry_timer);
        esp_timer_start_once(s_wifi_retry_timer, (uint64_t)delay * 1000);
        break;
    }
    default:
        break;
    }
}

static esp_err_t start_wifi(const gw_config_t *cfg)
{
    const gw_wifi_cfg_t *w = &cfg->network.wifi;
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
    base.route_prio = w->role == GW_WIFI_PRIMARY ? ROUTE_PRIO_WIFI_PRIMARY : ROUTE_PRIO_WIFI_BACKUP;
    base.flags = ipv6_flags(base.flags, &cfg->network.ipv6);
    const esp_netif_config_t nc = {.base = &base, .stack = ESP_NETIF_NETSTACK_DEFAULT_WIFI_STA};
    s_if[GW_NET_WIFI].netif = esp_netif_new(&nc);
    esp_netif_t *netif = s_if[GW_NET_WIFI].netif;
    ESP_RETURN_ON_FALSE(netif, ESP_FAIL, TAG, "wifi netif");
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(netif, cfg->network.hostname), TAG, "hostname");
    ESP_RETURN_ON_ERROR(esp_netif_attach_wifi_station(netif), TAG, "wifi attach");
    ESP_RETURN_ON_ERROR(esp_wifi_set_default_wifi_sta_handlers(), TAG, "wifi handlers");
    ESP_RETURN_ON_ERROR(configure_ipv4(netif, "wifi", w->ipv4.dhcp, w->ipv4.ip, w->ipv4.netmask,
                                       w->ipv4.gateway, w->ipv4.dns),
                        TAG, "wifi ip config");
    ESP_RETURN_ON_ERROR(configure_ipv6_dns(netif, &cfg->network.ipv6), TAG, "wifi ipv6 dns");

    const wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wc), TAG, "wifi init");
    /* the credentials live in config.json; do not keep a second copy in NVS */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    wifi_config_t sta = {0};
    memcpy(sta.sta.ssid, w->ssid, strnlen(w->ssid, sizeof(sta.sta.ssid)));
    memcpy(sta.sta.password, w->password, strnlen(w->password, sizeof(sta.sta.password)));
    sta.sta.threshold.authmode = w->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta.sta.pmf_cfg.capable = true;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta), TAG, "wifi config");

    const esp_timer_create_args_t ta = {.callback = wifi_connect, .name = "wifi_retry"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&ta, &s_wifi_retry_timer), TAG, "wifi timer");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL), TAG,
        "wifi events");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    /* latency of NCMD/DCMD matters more than the radio's power draw on a powered gateway */
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "wifi: station for '%s' (%s, failback after %u s)", w->ssid,
             w->role == GW_WIFI_PRIMARY ? "primary, Ethernet is the backup"
                                        : "backup of Ethernet",
             w->failback_s);
    return ESP_OK;
}
#endif

/* ---------------------------------------------------------------- start */

esp_err_t gw_net_start(const gw_config_t *cfg, const gw_net_callbacks_t *cb)
{
    s_cfg = cfg;
    s_cb = *cb;
    bool wifi = cfg->network.wifi.enabled;
#if !CONFIG_GW_WIFI
    if (wifi) {
        ESP_LOGW(TAG, "network.wifi ignored: Wi-Fi is not available in this build "
                      "(CONFIG_GW_WIFI); Ethernet only");
        wifi = false;
    }
#endif
    s_if_count = wifi ? 2 : 1;
    const uint8_t rank[GW_NET_IF_COUNT] = {
        [GW_NET_ETH] = cfg->network.wifi.role == GW_WIFI_PRIMARY ? 1 : 0,
        [GW_NET_WIFI] = cfg->network.wifi.role == GW_WIFI_PRIMARY ? 0 : 1,
    };
    gw_netsel_init(&s_sel, s_if_count, rank, (uint32_t)cfg->network.wifi.failback_s * 1000u);
    gw_netsel_set_hold(&s_sel, CONFIG_GW_NET_NO_SERVICE_MIN_S * 1000u,
                       CONFIG_GW_NET_NO_SERVICE_MAX_S * 1000u);
    if (cfg->network.ipv6.enabled) {
        ESP_LOGI(TAG, "IPv6 enabled (SLAAC %s%s)", cfg->network.ipv6.slaac ? "on" : "off",
                 cfg->network.ipv6.has_address ? ", static address on ethernet" : "");
    }
    publish_status();

    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, NULL),
                        TAG, "ip events");
    if (wifi) {
        const esp_timer_create_args_t ta = {.callback = on_poll_timer, .name = "net_failback"};
        ESP_RETURN_ON_ERROR(esp_timer_create(&ta, &s_poll_timer), TAG, "timer");
        ESP_RETURN_ON_ERROR(
            esp_event_handler_register(GW_NET_EVENT, ESP_EVENT_ANY_ID, on_gw_net_event, NULL),
            TAG, "net events");
    }

    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG(cfg->ntp.server);
    sc.start = false;            /* started once an address is available */
    sc.sync_cb = on_time_sync;
    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&sc), TAG, "sntp");

    ESP_RETURN_ON_ERROR(start_ethernet(cfg), TAG, "ethernet");
#if CONFIG_GW_WIFI
    if (wifi) {
        /* Ethernet keeps working if the radio cannot start */
        esp_err_t err = start_wifi(cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi unavailable (%s); Ethernet only", esp_err_to_name(err));
        }
    }
#endif
    return ESP_OK;
}
