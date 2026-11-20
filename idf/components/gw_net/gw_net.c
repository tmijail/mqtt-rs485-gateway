#include "gw_net.h"

#include <ctype.h>
#include <stdatomic.h>
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
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_backoff.h"
#include "gw_netsel.h"
#include "gw_rng.h"
#include "gw_storage.h"
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
static atomic_bool s_sntp_synced;   /* written in the lwIP task (on_time_sync) */

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

bool gw_net_ip4_info(gw_net_if_t i, uint8_t ip[4], uint8_t mask[4])
{
    esp_netif_ip_info_t info;
    if (i >= GW_NET_IF_COUNT || s_if[i].netif == NULL ||
        esp_netif_get_ip_info(s_if[i].netif, &info) != ESP_OK || info.ip.addr == 0) {
        return false;
    }
    memcpy(ip, &info.ip.addr, 4);
    memcpy(mask, &info.netmask.addr, 4);
    return true;
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

_Static_assert(CONFIG_LWIP_SNTP_MAX_SERVERS >= GW_MAX_NTP_SERVERS,
               "#201: the SNTP client must get all the servers of ntp.servers");

static void on_time_sync(struct timeval *tv)
{
    (void)tv;
    atomic_store(&s_sntp_synced, true);
    s_cb.time_synced();
}

/* SNTP starts with the first address. Issue #160: an address that comes before the one the
 * server needs (with network.ipv6, the static IPv6 address comes before the DHCP IPv4 address)
 * makes the first request fail (no route to an IPv4 server), and lwIP sends the next one only
 * after its retry interval (15 s or more). Without a valid clock the gateway publishes no NDATA
 * or DDATA, so until the first synchronisation each new address starts SNTP again
 * (esp_netif_sntp_start() restarts it when it runs): one request at once. */
static void sntp_on_address(void)
{
    if (!s_sntp_started || !atomic_load(&s_sntp_synced)) {
        s_sntp_started = esp_netif_sntp_start() == ESP_OK;
    }
}

static void got_ipv4(gw_net_if_t i, const ip_event_got_ip_t *ev)
{
    ESP_LOGI(TAG, "%sIPv4 " IPSTR " mask " IPSTR " gw " IPSTR, i == GW_NET_WIFI ? "wifi: " : "",
             IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.netmask), IP2STR(&ev->ip_info.gw));
    sntp_on_address();
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
        sntp_on_address();
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
_Static_assert(GW_WIFI_MODE_STA == (int)WIFI_MODE_STA && GW_WIFI_MODE_AP == (int)WIFI_MODE_AP &&
                   GW_WIFI_MODE_APSTA == (int)WIFI_MODE_APSTA,
               "gw_wifi_mode_t has the values of wifi_mode_t");

/* Issue #208: the users of the driver (gw_wifiown: the station, the access point of gw_ap, a
 * scan), under s_wifi_lock. Only this code initialises, starts, stops and deinitialises it. */
static uint8_t s_wifi_users;
static SemaphoreHandle_t s_wifi_lock;
static StaticSemaphore_t s_wifi_lock_buf;
static atomic_int s_wifi_lock_state;   /* 0: not made, 1: in creation, 2: ready */

/* The lock, made at the first use: gw_ap can come before gw_net_start. */
static SemaphoreHandle_t wifi_lock(void)
{
    int none = 0;
    if (atomic_load(&s_wifi_lock_state) != 2) {
        if (atomic_compare_exchange_strong(&s_wifi_lock_state, &none, 1)) {
            s_wifi_lock = xSemaphoreCreateMutexStatic(&s_wifi_lock_buf);
            atomic_store(&s_wifi_lock_state, 2);
        }
        while (atomic_load(&s_wifi_lock_state) != 2) {
            vTaskDelay(1);
        }
    }
    return s_wifi_lock;
}

static esp_err_t wifi_apply(gw_wifiown_plan_t p, esp_err_t (*configure)(void *ctx), void *ctx)
{
    if (p.init) {
        wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
        /* issue #267: the radio of the RNG is a station that never connects. With the default
         * (CONFIG_ESP_WIFI_STA_DISCONNECTED_PM_ENABLE) a station without a connection goes into
         * modem sleep, and the RF subsystem is then no entropy source */
        wc.sta_disconnected_pm = false;
        ESP_RETURN_ON_ERROR(esp_wifi_init(&wc), TAG, "wifi init");
        /* the credentials live in config.json; do not keep a second copy in NVS */
        ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage");
    }
    if (p.set_mode) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode((wifi_mode_t)p.mode), TAG, "wifi mode");
    }
    if (configure != NULL) {
        ESP_RETURN_ON_ERROR(configure(ctx), TAG, "wifi config");
    }
    if (p.start) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    }
    if (p.stop) {
        esp_err_t e = esp_wifi_stop();
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "wifi: esp_wifi_stop: %s", esp_err_to_name(e));
        }
    }
    if (p.deinit) {
        ESP_RETURN_ON_ERROR(esp_wifi_deinit(), TAG, "wifi deinit");
    }
    return ESP_OK;
}

static const char *user_name(gw_wifi_user_t u)
{
    return u == GW_WIFI_USER_STA  ? "station"
           : u == GW_WIFI_USER_AP ? "access point"
           : u == GW_WIFI_USER_RNG ? "entropy source"
                                   : "scan";
}

esp_err_t gw_net_wifi_acquire(gw_wifi_user_t user, esp_err_t (*configure)(void *ctx), void *ctx)
{
    SemaphoreHandle_t l = wifi_lock();
    xSemaphoreTake(l, portMAX_DELAY);
    uint8_t before = s_wifi_users, after = (uint8_t)(before | user);
    gw_wifiown_plan_t p = gw_wifiown_plan(before, after);
    if (p.refuse) {
        ESP_LOGE(TAG, "wifi: the station cannot start after the STA interface (gw_wifiown)");
        xSemaphoreGive(l);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t e = wifi_apply(p, configure, ctx);
    if (e == ESP_OK) {
        s_wifi_users = after;
        if (p.start) {
            ESP_LOGI(TAG, "wifi: driver started for the %s", user_name(user));
        }
    } else if (before == 0) {
        esp_wifi_stop();   /* back to no driver, as before the call */
        esp_wifi_deinit();
    } else if (p.set_mode) {
        esp_wifi_set_mode((wifi_mode_t)gw_wifiown_mode(before));
    }
    xSemaphoreGive(l);
    return e;
}

void gw_net_wifi_release(gw_wifi_user_t user)
{
    SemaphoreHandle_t l = wifi_lock();
    xSemaphoreTake(l, portMAX_DELAY);
    if ((s_wifi_users & user) != 0) {
        uint8_t after = (uint8_t)(s_wifi_users & ~user);
        gw_wifiown_plan_t p = gw_wifiown_plan(s_wifi_users, after);
        esp_err_t e = wifi_apply(p, NULL, NULL);
        s_wifi_users = after;
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "wifi: the %s did not give the driver back: %s", user_name(user),
                     esp_err_to_name(e));
        } else if (p.deinit) {
            ESP_LOGI(TAG, "wifi: driver stopped and released (the %s was the last user)",
                     user_name(user));
        }
    }
    xSemaphoreGive(l);
}

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

/* The configuration of the station, after the mode and before the start of the driver. */
static esp_err_t sta_configure(void *ctx)
{
    const gw_wifi_cfg_t *w = ctx;
    wifi_config_t sta = {0};
    memcpy(sta.sta.ssid, w->ssid, strnlen(w->ssid, sizeof(sta.sta.ssid)));
    memcpy(sta.sta.password, w->password, strnlen(w->password, sizeof(sta.sta.password)));
    sta.sta.threshold.authmode = w->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta.sta.pmf_cfg.capable = true;
    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &sta);
    memset(&sta, 0, sizeof(sta));
    return e;
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

    const esp_timer_create_args_t ta = {.callback = wifi_connect, .name = "wifi_retry"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&ta, &s_wifi_retry_timer), TAG, "wifi timer");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL), TAG,
        "wifi events");
    /* issue #208: the driver starts here (STA_START connects), or, with the access point of a
     * BOOT gesture already on, gets the station interface (APSTA) */
    ESP_RETURN_ON_ERROR(gw_net_wifi_acquire(GW_WIFI_USER_STA, sta_configure, (void *)w), TAG,
                        "wifi start");
    /* latency of NCMD/DCMD matters more than the radio's power draw on a powered gateway; and
     * issue #267: the RF subsystem is an entropy source only when it does not sleep */
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "wifi: station for '%s' (%s, failback after %u s)", w->ssid,
             w->role == GW_WIFI_PRIMARY ? "primary, Ethernet is the backup"
                                        : "backup of Ethernet",
             w->failback_s);
    return ESP_OK;
}
#endif

#if CONFIG_GW_WIFI
/* Issue #267: no power save, so that the RF subsystem stays on (an entropy source). Before the
 * start of the driver when the radio of the RNG starts it. */
static esp_err_t rng_configure(void *ctx)
{
    (void)ctx;
    return esp_wifi_set_ps(WIFI_PS_NONE);
}
#endif

/* Issue #267 (gw_rng.h, step 3): the radio as the entropy source of the RNG, until the restart.
 * After the station of network.wifi (gw_wifiown: the station comes first). With no station the
 * Wi-Fi driver starts here in STA mode: no configuration, no esp_wifi_connect(), no access point.
 * The scan and the access point then only add and remove their interface. */
static void start_rng_radio(void)
{
#if CONFIG_GW_WIFI
    esp_err_t e = gw_net_wifi_acquire(GW_WIFI_USER_RNG, rng_configure, NULL);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "wifi: the radio of the entropy source did not start (%s)",
                 esp_err_to_name(e));
    }
    gw_rng_radio_on(e == ESP_OK);
#else
    ESP_LOGW(TAG, "no radio in this build (CONFIG_GW_WIFI): no entropy source after the SAR ADC "
                  "source (issue #267)");
#endif
}

/* ---------------------------------------------------------------- Wi-Fi scan (#208) */

#if CONFIG_GW_WIFI || CONFIG_GW_IO_SIMULATED
/* The result of the last scan, in PSRAM (allocated once in gw_net_start), under s_scan_lock.
 * The scan works also when network.wifi is off: the button "Find networks" of the web UI. */
typedef struct {
    gw_net_scan_state_t state;
    bool simulated;
    int64_t done_us;
    uint8_t count;
    char error[64];
    gw_net_scan_ap_t ap[GW_NET_SCAN_MAX];
} scan_t;

static scan_t *s_scan;
static SemaphoreHandle_t s_scan_lock;
static StaticSemaphore_t s_scan_lock_buf;

/* Keep the strongest record of each SSID, strongest first; no hidden networks. Under the lock. */
static void scan_add(const char *ssid, int8_t rssi, uint8_t channel, const char *security,
                     bool supported)
{
    if (ssid[0] == '\0') {
        return;
    }
    int at = -1;
    for (int i = 0; i < s_scan->count; i++) {
        if (strcmp(s_scan->ap[i].ssid, ssid) == 0) {
            if (s_scan->ap[i].rssi >= rssi) {
                return;
            }
            memmove(&s_scan->ap[i], &s_scan->ap[i + 1],
                    (size_t)(s_scan->count - i - 1) * sizeof(s_scan->ap[0]));
            s_scan->count--;
            break;
        }
    }
    for (int i = 0; i < s_scan->count && at < 0; i++) {
        at = rssi > s_scan->ap[i].rssi ? i : -1;
    }
    if (at < 0) {
        if (s_scan->count >= GW_NET_SCAN_MAX) {
            return;
        }
        at = s_scan->count;
    } else {
        int keep = s_scan->count < GW_NET_SCAN_MAX ? s_scan->count : GW_NET_SCAN_MAX - 1;
        memmove(&s_scan->ap[at + 1], &s_scan->ap[at], (size_t)(keep - at) * sizeof(s_scan->ap[0]));
    }
    gw_net_scan_ap_t *a = &s_scan->ap[at];
    snprintf(a->ssid, sizeof(a->ssid), "%s", ssid);
    a->rssi = rssi;
    a->channel = channel;
    a->security = security;
    a->supported = supported;
    if (s_scan->count < GW_NET_SCAN_MAX) {
        s_scan->count++;
    }
}

static void scan_finish(gw_net_scan_state_t state, const char *error)
{
    s_scan->state = state;
    s_scan->done_us = esp_timer_get_time();
    snprintf(s_scan->error, sizeof(s_scan->error), "%s", error ? error : "");
    ESP_LOGI(TAG, "wifi scan: %s%s%s (%u networks)", state == GW_NET_SCAN_DONE ? "done" : "failed",
             error ? ": " : "", error ? error : "", s_scan->count);
}

#if CONFIG_GW_WIFI
static bool s_scan_radio;   /* the scan is a user of the driver (gw_net_wifi_acquire) */
static esp_timer_handle_t s_scan_end_timer;

/* The name of the security of a network, and whether the station of the gateway can join it:
 * open or WPA2/WPA3 personal (network.wifi.password). No enterprise (802.1X), no WEP or WPA. */
static const char *auth_name(wifi_auth_mode_t m, bool *supported)
{
    *supported = false;
    switch (m) {
    case WIFI_AUTH_OPEN:
        *supported = true;
        return "Open";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA";
    case WIFI_AUTH_WPA2_PSK:
        *supported = true;
        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        *supported = true;
        return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:
        *supported = true;
        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        *supported = true;
        return "WPA2/WPA3";
    case WIFI_AUTH_WPA_ENTERPRISE:
        return "WPA-Enterprise";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA3_ENT_192:
        return "WPA3-Enterprise";
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:
        return "WPA2/WPA3-Enterprise";
    case WIFI_AUTH_WAPI_PSK:
        return "WAPI";
    case WIFI_AUTH_OWE:
        return "OWE";
    default:
        return "Unknown";
    }
}

/* Back to the radio state from before the scan (gw_wifiown: the scan gives the driver back; the
 * radio of the entropy source keeps it, issue #267). Under s_scan_lock. */
static void scan_radio_release(void)
{
    if (s_scan_radio) {
        gw_net_wifi_release(GW_WIFI_USER_SCAN);
        s_scan_radio = false;
    }
}

/* The esp_timer task, just after WIFI_EVENT_SCAN_DONE: not in the event loop task, because
 * esp_wifi_stop() waits for the Wi-Fi task, which posts its events to that loop. A new scan
 * that started in between keeps the radio. */
static void scan_end(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    if (s_scan->state != GW_NET_SCAN_RUNNING) {
        scan_radio_release();
    }
    xSemaphoreGive(s_scan_lock);
}

/* WIFI_EVENT_SCAN_DONE, in the default event loop task: one record at a time (no big buffer). */
static void on_scan_done(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    const wifi_event_sta_scan_done_t *ev = data;
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    if (s_scan->state == GW_NET_SCAN_RUNNING) {
        s_scan->count = 0;
        wifi_ap_record_t rec;
        while (esp_wifi_scan_get_ap_record(&rec) == ESP_OK) {
            bool ok;
            const char *sec = auth_name(rec.authmode, &ok);
            scan_add((const char *)rec.ssid, rec.rssi, rec.primary, sec, ok);
        }
        esp_wifi_clear_ap_list();
        scan_finish(ev != NULL && ev->status != 0 ? GW_NET_SCAN_FAILED : GW_NET_SCAN_DONE,
                    ev != NULL && ev->status != 0 ? "the radio stopped the scan" : NULL);
        esp_timer_stop(s_scan_end_timer);
        esp_timer_start_once(s_scan_end_timer, 0);   /* scan_end gives the radio back */
    }
    xSemaphoreGive(s_scan_lock);
}

/* The radio for a scan: the scan becomes a user of the driver (APSTA with the access point; no
 * change in STA mode: the station or the radio of the entropy source, issue #267). Under the
 * lock. */
static esp_err_t scan_radio_start(void)
{
    static bool handler;
    if (!handler) {
        ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                                       on_scan_done, NULL),
                            TAG, "scan event");
        handler = true;
    }
    if (!s_scan_radio) {
        ESP_RETURN_ON_ERROR(gw_net_wifi_acquire(GW_WIFI_USER_SCAN, NULL, NULL), TAG, "radio");
        s_scan_radio = true;
    }
    return ESP_OK;
}
#endif

#if CONFIG_GW_IO_SIMULATED && !CONFIG_GW_WIFI
/* QEMU has no radio: a fixed list after 1.5 s, as a real scan takes approximately 3 s */
static esp_timer_handle_t s_scan_sim_timer;

static void scan_sim_done(void *arg)
{
    (void)arg;
    static const struct {
        const char *ssid;
        int8_t rssi;
        uint8_t channel;
        const char *security;
        bool supported;
    } k_sim[] = {
        {"Plant-IoT", -48, 6, "WPA2", true},          {"Plant-Office", -61, 1, "WPA2/WPA3", true},
        {"Plant-Corp", -66, 11, "WPA2-Enterprise", false}, {"Warehouse-AP2", -71, 6, "WPA2", true},
        {"Guest", -74, 1, "Open", true},              {"DIRECT-7F-HP M479", -83, 11, "WPA2", true},
        {"Plant-IoT", -80, 11, "WPA2", true},         /* a second access point of Plant-IoT */
        {"", -50, 3, "WPA2", true},                   /* a hidden network */
    };
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    s_scan->count = 0;
    for (size_t i = 0; i < sizeof(k_sim) / sizeof(k_sim[0]); i++) {
        scan_add(k_sim[i].ssid, k_sim[i].rssi, k_sim[i].channel, k_sim[i].security,
                 k_sim[i].supported);
    }
    scan_finish(GW_NET_SCAN_DONE, NULL);
    xSemaphoreGive(s_scan_lock);
}
#endif

gw_net_scan_state_t gw_net_wifi_scan_start(uint32_t max_age_s)
{
    if (s_scan == NULL) {
        return GW_NET_SCAN_UNAVAILABLE;
    }
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    gw_net_scan_state_t st = s_scan->state;
    int64_t age_us = esp_timer_get_time() - s_scan->done_us;
    if (st == GW_NET_SCAN_RUNNING ||
        (st == GW_NET_SCAN_DONE && age_us < (int64_t)max_age_s * 1000000)) {
        xSemaphoreGive(s_scan_lock);
        return st;
    }
    s_scan->state = GW_NET_SCAN_RUNNING;
    s_scan->count = 0;
#if CONFIG_GW_WIFI
    const wifi_scan_config_t sc = {.show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE};
    esp_err_t e = scan_radio_start();
    if (e == ESP_OK) {
        e = esp_wifi_scan_start(&sc, false);
    }
    if (e != ESP_OK) {
        char why[64];
        snprintf(why, sizeof(why), "the radio cannot scan now (%s)", esp_err_to_name(e));
        scan_finish(GW_NET_SCAN_FAILED, why);
        scan_radio_release();
    } else {
        ESP_LOGI(TAG, "wifi scan started");
    }
#else
    s_scan->simulated = true;
    ESP_LOGI(TAG, "wifi scan started (simulated: QEMU has no radio)");
    esp_timer_start_once(s_scan_sim_timer, 1500 * 1000);
#endif
    st = s_scan->state;
    xSemaphoreGive(s_scan_lock);
    return st;
}

gw_net_scan_state_t gw_net_wifi_scan_result(gw_net_scan_cb_t cb, void *ctx, uint32_t *age_s,
                                            bool *simulated, char *error, size_t error_cap)
{
    if (s_scan == NULL) {
        return GW_NET_SCAN_UNAVAILABLE;
    }
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    gw_net_scan_state_t st = s_scan->state;
    *age_s = (uint32_t)((esp_timer_get_time() - s_scan->done_us) / 1000000);
    *simulated = s_scan->simulated;
    snprintf(error, error_cap, "%s", s_scan->error);
    for (uint8_t i = 0; st == GW_NET_SCAN_DONE && cb != NULL && i < s_scan->count; i++) {
        cb(ctx, &s_scan->ap[i]);
    }
    xSemaphoreGive(s_scan_lock);
    return st;
}

static void scan_init(void)
{
    s_scan = gw_storage_calloc(1, sizeof(*s_scan));
    if (s_scan == NULL) {
        ESP_LOGE(TAG, "wifi scan: no memory for the result");
        return;
    }
    s_scan_lock = xSemaphoreCreateMutexStatic(&s_scan_lock_buf);
#if CONFIG_GW_IO_SIMULATED && !CONFIG_GW_WIFI
    const esp_timer_create_args_t ta = {.callback = scan_sim_done, .name = "wifi_scan_sim"};
    if (esp_timer_create(&ta, &s_scan_sim_timer) != ESP_OK) {
        s_scan = NULL;
    }
#endif
#if CONFIG_GW_WIFI
    const esp_timer_create_args_t te = {.callback = scan_end, .name = "wifi_scan_end"};
    if (esp_timer_create(&te, &s_scan_end_timer) != ESP_OK) {
        s_scan = NULL;   /* no scan: the radio would not stop after it */
    }
#endif
}
#else   /* no radio and not the emulator */
gw_net_scan_state_t gw_net_wifi_scan_start(uint32_t max_age_s)
{
    (void)max_age_s;
    return GW_NET_SCAN_UNAVAILABLE;
}

gw_net_scan_state_t gw_net_wifi_scan_result(gw_net_scan_cb_t cb, void *ctx, uint32_t *age_s,
                                            bool *simulated, char *error, size_t error_cap)
{
    (void)cb;
    (void)ctx;
    *age_s = 0;
    *simulated = false;
    snprintf(error, error_cap, "this build has no Wi-Fi radio");
    return GW_NET_SCAN_UNAVAILABLE;
}

static void scan_init(void)
{
}
#endif

/* ---------------------------------------------------------------- start */

esp_err_t gw_net_start(const gw_config_t *cfg, const gw_net_callbacks_t *cb)
{
    s_cfg = cfg;
    s_cb = *cb;
    scan_init();
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

    /* #201: all the servers; lwIP asks the next one when a server does not answer in
     * SNTP_RECV_TIMEOUT (15 s) */
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG(cfg->ntp.servers[0]);
    sc.num_of_servers = cfg->ntp.server_count;
    for (uint8_t i = 0; i < cfg->ntp.server_count; i++) {
        sc.servers[i] = cfg->ntp.servers[i];
    }
    sc.start = false;            /* started once an address is available */
    sc.sync_cb = on_time_sync;
    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&sc), TAG, "sntp");

    esp_err_t eth = start_ethernet(cfg);
    if (eth != ESP_OK) {
        ESP_LOGE(TAG, "ethernet unavailable (%s)", esp_err_to_name(eth));
    }
    /* issue #267: the SAR ADC entropy source goes off before the Wi-Fi driver starts (gw_rng.h);
     * also in a build with no radio, so that the sequence is the same. The radio of the entropy
     * source starts also when Ethernet failed. */
    gw_rng_radio_starts();
#if CONFIG_GW_WIFI
    if (wifi && eth == ESP_OK) {
        /* Ethernet keeps working if the radio cannot start */
        esp_err_t err = start_wifi(cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi unavailable (%s); Ethernet only", esp_err_to_name(err));
        }
    }
#endif
    start_rng_radio();
    return eth;
}
