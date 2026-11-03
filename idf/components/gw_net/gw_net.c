#include "gw_net.h"

#include <string.h>

#include "esp_check.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "lwip/ip4_addr.h"

#if CONFIG_GW_ETH_W5500
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#endif

static const char *TAG = "net";

static gw_net_callbacks_t s_cb;
static esp_netif_t *s_netif;
static bool s_sntp_started;

static esp_ip4_addr_t ip4(const uint8_t a[4])
{
    esp_ip4_addr_t r;
    IP4_ADDR(&r, a[0], a[1], a[2], a[3]);
    return r;
}

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == ETHERNET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "link up");
    } else if (id == ETHERNET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "link down");
        s_cb.ip_changed(false);
    }
}

static void on_time_sync(struct timeval *tv)
{
    (void)tv;
    s_cb.time_synced();
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == IP_EVENT_ETH_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "IPv4 " IPSTR " mask " IPSTR " gw " IPSTR, IP2STR(&ev->ip_info.ip),
                 IP2STR(&ev->ip_info.netmask), IP2STR(&ev->ip_info.gw));
        if (!s_sntp_started) {
            s_sntp_started = esp_netif_sntp_start() == ESP_OK;
        }
        s_cb.ip_changed(true);
    } else if (id == IP_EVENT_ETH_LOST_IP) {
        ESP_LOGW(TAG, "IPv4 lost");
        s_cb.ip_changed(false);
    }
}

static esp_err_t configure_ip(const gw_config_t *cfg)
{
    if (cfg->network.dhcp) {
        return ESP_OK;
    }
    esp_err_t err = esp_netif_dhcpc_stop(s_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return err;
    }
    const esp_netif_ip_info_t info = {
        .ip = ip4(cfg->network.ip),
        .netmask = ip4(cfg->network.netmask),
        .gw = ip4(cfg->network.gateway),
    };
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_netif, &info), TAG, "static ip");
    const uint8_t *d = cfg->network.dns;
    if (d[0] | d[1] | d[2] | d[3]) {
        esp_netif_dns_info_t dns = {0};
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4 = ip4(d);
        ESP_RETURN_ON_ERROR(esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns), TAG, "dns");
    }
    ESP_LOGI(TAG, "static IPv4 %u.%u.%u.%u", cfg->network.ip[0], cfg->network.ip[1],
             cfg->network.ip[2], cfg->network.ip[3]);
    return ESP_OK;
}

static esp_err_t new_driver(esp_eth_handle_t *eth)
{
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
#if CONFIG_GW_ETH_W5500
    ESP_RETURN_ON_ERROR(gpio_install_isr_service(0), TAG, "isr service");
    const spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_GW_ETH_SPI_MOSI_GPIO,
        .miso_io_num = CONFIG_GW_ETH_SPI_MISO_GPIO,
        .sclk_io_num = CONFIG_GW_ETH_SPI_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");
    spi_device_interface_config_t dev = {
        .mode = 0,
        .clock_speed_hz = CONFIG_GW_ETH_SPI_MHZ * 1000 * 1000,
        .spics_io_num = CONFIG_GW_ETH_SPI_CS_GPIO,
        .queue_size = 20,
    };
    eth_w5500_config_t w5500 = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &dev);
    w5500.base.int_gpio_num = CONFIG_GW_ETH_INT_GPIO;
    phy_cfg.reset_gpio_num = CONFIG_GW_ETH_RST_GPIO;
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

esp_err_t gw_net_start(const gw_config_t *cfg, const gw_net_callbacks_t *cb)
{
    s_cb = *cb;
    const esp_netif_config_t nc = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&nc);
    ESP_RETURN_ON_FALSE(s_netif, ESP_FAIL, TAG, "netif");
    ESP_RETURN_ON_ERROR(configure_ip(cfg), TAG, "ip config");

    esp_eth_handle_t eth;
    ESP_RETURN_ON_ERROR(new_driver(&eth), TAG, "driver");
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_netif, esp_eth_new_netif_glue(eth)), TAG, "attach");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL),
                        TAG, "eth events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, NULL),
                        TAG, "ip events");

    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG(cfg->ntp.server);
    sc.start = false;            /* started once an address is available */
    sc.sync_cb = on_time_sync;
    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&sc), TAG, "sntp");

    return esp_eth_start(eth);
}
