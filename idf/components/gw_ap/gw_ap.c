#include "gw_ap.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gw_apdns.h"
#include "gw_net.h"
#include "gw_wpa.h"
#include "sdkconfig.h"

static const char *TAG = "ap";

#if CONFIG_GW_SOFTAP || CONFIG_GW_IO_SIMULATED
#define AP_AVAILABLE 1
#else
#define AP_AVAILABLE 0
#endif

#define TICK_MS        1000u   /* the life of the access point is looked at once a second */
#define STOP_SOON_MS   1500u   /* POST /api/ap/stop: the answer goes out first */

void gw_ap_ssid(char out[33])
{
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
        out[0] = '\0';
        return;
    }
    gw_wpa_ssid(mac, out);
}

#if AP_AVAILABLE

/* The state is shared by the button task (start), the web server and the shell (stop, status),
 * the esp_timer task (the tick) and the caller of gw_ap_claimed: under s_lock. The callback of
 * the caller is called without the lock. */
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static esp_timer_handle_t s_tick;
static void (*s_changed)(bool running, bool open, gw_ap_stop_t why);
static gw_aplife_t s_life;
static char s_key[GW_AP_KEY_SIZE];   /* passphrase, or a PMK of 64 hexadecimal characters */
static bool s_key_is_pmk;
static char s_ssid[33];
static int s_dns_netif = -1;   /* issue #274: the lwIP interface of the DNS server, -1 = off */

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static const char *key_name(void)
{
    return s_key_is_pmk ? "the web password" : "the commissioning password";
}

/* ---------- the radio: the real access point, or the emulator's model ---------- */

#if CONFIG_GW_SOFTAP
#include "esp_netif.h"
#include "esp_wifi.h"

static esp_netif_t *s_netif;

/* The configuration of the access point: WPA2-PSK with s_key, or open (no key) during the open
 * period of a claim window. A change while it runs disconnects its clients. */
static esp_err_t radio_apply(bool open)
{
    wifi_config_t ap = {0};
    size_t n = strnlen(s_ssid, sizeof(ap.ap.ssid));
    memcpy(ap.ap.ssid, s_ssid, n);
    ap.ap.ssid_len = (uint8_t)n;
    if (open) {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        /* issue #187: a PMK fills the 64 bytes of the field with no NUL; the driver takes 64
         * hexadecimal characters as the PMK itself (hostapd_derive_psk: no PBKDF2). WPA2 only:
         * WPA3 (SAE) needs the passphrase */
        size_t kl = strnlen(s_key, sizeof(ap.ap.password));
        memcpy(ap.ap.password, s_key, kl);
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ap.ap.pmf_cfg.capable = true;
    }
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    esp_err_t e = esp_wifi_set_config(WIFI_IF_AP, &ap);
    memset(&ap, 0, sizeof(ap));
    return e;
}

static esp_err_t ap_configure(void *ctx)
{
    return radio_apply(*(const bool *)ctx);
}

/* Issue #208: gw_net owns the driver (gw_wifiown). The access point is one of its users: it gets
 * the AP interface (APSTA) next to the station, a scan or the radio of the entropy source (issue
 * #267), or the driver starts in AP mode when it is off (only before gw_net_start). */
static esp_err_t radio_start(bool open)
{
    if (s_netif == NULL) {
        s_netif = esp_netif_create_default_wifi_ap();
        ESP_RETURN_ON_FALSE(s_netif != NULL, ESP_FAIL, TAG, "AP netif");
    }
    return gw_net_wifi_acquire(GW_WIFI_USER_AP, ap_configure, &open);
}

/* The station, a scan and the radio of the entropy source go on (issue #267: the driver goes
 * back to STA mode and does not stop). */
static void radio_stop(void)
{
    gw_net_wifi_release(GW_WIFI_USER_AP);
}

static unsigned radio_clients(void)
{
    wifi_sta_list_t list;
    return esp_wifi_ap_get_sta_list(&list) == ESP_OK ? (unsigned)list.num : 0;
}

static void radio_addr(char *out, size_t cap)
{
    snprintf(out, cap, "?");
    esp_netif_ip_info_t info;
    if (s_netif != NULL && esp_netif_get_ip_info(s_netif, &info) == ESP_OK && info.ip.addr != 0) {
        snprintf(out, cap, IPSTR, IP2STR(&info.ip));
    }
}

/* Issue #274: the lwIP interface of the captive portal: the interface of the access point. */
static int portal_netif(char *addr, size_t cap)
{
    radio_addr(addr, cap);
    return s_netif != NULL ? esp_netif_get_netif_impl_index(s_netif) : -1;
}

bool gw_ap_ip4(uint8_t ip[4], uint8_t mask[4])
{
    esp_netif_ip_info_t info;
    if (!gw_ap_active() || s_netif == NULL || esp_netif_get_ip_info(s_netif, &info) != ESP_OK) {
        return false;
    }
    memcpy(ip, &info.ip.addr, 4);
    memcpy(mask, &info.netmask.addr, 4);
    return true;
}

#if CONFIG_GW_IO_SIMULATED
void gw_ap_sim_clients(unsigned n)
{
    (void)n;   /* a real radio counts its own clients */
}

void gw_ap_sim_idle_s(uint32_t s)
{
    (void)s;
}

void gw_ap_sim_eth_as_ap(bool eth_as_ap)
{
    (void)eth_as_ap;   /* the radio has its own interface */
}
#endif

#else   /* CONFIG_GW_IO_SIMULATED: QEMU has no radio */

static volatile unsigned s_sim_clients;
static bool s_sim_eth_as_ap;   /* under s_lock */

static esp_err_t radio_apply(bool open)
{
    (void)open;
    return ESP_OK;
}

static esp_err_t radio_start(bool open)
{
    (void)open;
    return ESP_OK;
}

static void radio_stop(void)
{
}

static unsigned radio_clients(void)
{
    return s_sim_clients;
}

static void radio_addr(char *out, size_t cap)
{
    snprintf(out, cap, "(no radio)");
}

/* Issue #274: QEMU has no radio: with 'claimsim ... eth' the captive portal is on the emulated
 * Ethernet (the interface that counts as the access point for the claim window); else nowhere. */
static int portal_netif(char *addr, size_t cap)
{
    uint8_t ip[4], mask[4];
    if (!s_sim_eth_as_ap || !gw_net_ip4_info(GW_NET_ETH, ip, mask)) {
        return -1;
    }
    snprintf(addr, cap, "%u.%u.%u.%u (the Ethernet: claimsim ... eth)", ip[0], ip[1], ip[2], ip[3]);
    return gw_net_lwip_index(GW_NET_ETH);
}

bool gw_ap_ip4(uint8_t ip[4], uint8_t mask[4])
{
    (void)ip;
    (void)mask;
    return false;   /* no netif: 'claimsim ... eth' makes the Ethernet count as the access point */
}

void gw_ap_sim_clients(unsigned n)
{
    s_sim_clients = n;
    ESP_LOGI(TAG, "sim: %u client(s) on the access point", n);
}

static void dns_follow(void);

void gw_ap_sim_eth_as_ap(bool eth_as_ap)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sim_eth_as_ap = eth_as_ap;
    dns_follow();
    xSemaphoreGive(s_lock);
}

void gw_ap_sim_idle_s(uint32_t s)
{
    ESP_LOGI(TAG, "sim: the access point stops after %lu s without a client",
             (unsigned long)(s != 0 ? s : CONFIG_GW_SOFTAP_IDLE_MIN * 60u));
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_life.idle_ms = (s != 0 ? s : CONFIG_GW_SOFTAP_IDLE_MIN * 60u) * 1000u;
        xSemaphoreGive(s_lock);
    }
}
#endif

/* ---------- issue #274: the DNS server of the captive portal ---------- */

/* The DNS server runs while the access point runs, on its interface (gw_apdns.c). Under s_lock. */
static void dns_follow(void)
{
    char addr[64] = "?";
    int idx = s_life.running ? portal_netif(addr, sizeof(addr)) : -1;
    if (idx == s_dns_netif) {
        return;
    }
    if (s_dns_netif > 0) {
        gw_apdns_stop();
        s_dns_netif = -1;
        uint32_t answered, dropped;
        gw_apdns_counts(&answered, &dropped);
        ESP_LOGI(TAG, "captive portal: DNS server stopped (since boot: %lu answers, %lu queries "
                      "without an answer)", (unsigned long)answered, (unsigned long)dropped);
    }
    if (idx <= 0) {
        return;
    }
    esp_err_t e = gw_apdns_start(idx);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "captive portal: DNS server not started (%s): a phone does not open the "
                      "guide page by itself", esp_err_to_name(e));
        return;
    }
    s_dns_netif = idx;
    ESP_LOGI(TAG, "captive portal: the DNS server answers each A query of a client of the access "
                  "point with %s", addr);
}

/* ---------- the life (gw_aplife), under s_lock ---------- */

static void notify(bool running, bool open, gw_ap_stop_t why)
{
    if (s_changed != NULL) {
        s_changed(running, open, why);
    }
}

/* Apply an action of the life; under s_lock. True when the caller must notify. */
static bool apply_action(gw_aplife_action_t a, gw_ap_stop_t why)
{
    if (a == GW_APLIFE_STOP) {
        esp_timer_stop(s_tick);
        dns_follow();
        radio_stop();
        ESP_LOGW(TAG, "commissioning access point '%s' stopped (%s)", s_ssid,
                 why == GW_AP_STOP_IDLE ? "no client for the idle time" : gw_ap_stop_text(why));
        return true;
    }
    if (a == GW_APLIFE_SECURE) {
        esp_err_t e = radio_apply(false);
        if (e == ESP_OK) {
            ESP_LOGW(TAG, "access point '%s' uses WPA2 with %s (the open period is over)", s_ssid,
                     key_name());
        } else {
            ESP_LOGE(TAG, "access point '%s' not changed to WPA2 (%s): stopped", s_ssid,
                     esp_err_to_name(e));
            gw_aplife_stop(&s_life, GW_AP_STOP_REQUEST);
            esp_timer_stop(s_tick);
            dns_follow();
            radio_stop();
        }
        return true;
    }
    return false;
}

static void tick(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint64_t now = now_ms();
    if (s_life.running) {
        unsigned n = radio_clients();
        if (n != s_life.clients) {
            ESP_LOGI(TAG, "access point '%s': %u client(s)", s_ssid, n);
        }
        gw_aplife_clients(&s_life, n, now);
    }
    gw_ap_stop_t why;
    gw_aplife_action_t a = gw_aplife_tick(&s_life, now, &why);
    bool changed = apply_action(a, why);
    bool running = s_life.running, open = s_life.open;
    gw_ap_stop_t last = s_life.stopped;
    xSemaphoreGive(s_lock);
    if (changed) {
        notify(running, open, last);
    }
}

esp_err_t gw_ap_init(void (*changed)(bool running, bool open, gw_ap_stop_t why))
{
    if (s_lock != NULL) {
        return ESP_OK;
    }
    s_changed = changed;
    gw_aplife_init(&s_life, CONFIG_GW_SOFTAP_IDLE_MIN * 60u * 1000u);
    const esp_timer_create_args_t ta = {.callback = tick, .name = "ap_life"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&ta, &s_tick), TAG, "timer");
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    return ESP_OK;
}

esp_err_t gw_ap_start(const char *key, bool key_is_pmk, uint32_t max_s, bool https,
                      uint16_t web_port, uint32_t open_s)
{
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "gw_ap_init not called");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was_running = s_life.running, was_open = s_life.open;
    bool new_key = strncmp(s_key, key, sizeof(s_key)) != 0;
    snprintf(s_key, sizeof(s_key), "%s", key);
    s_key_is_pmk = key_is_pmk;
    bool open = open_s != 0;
    esp_err_t e = ESP_OK;
    if (!was_running) {
        gw_ap_ssid(s_ssid);
        if (s_ssid[0] == '\0') {
            e = ESP_ERR_INVALID_MAC;
        } else {
            e = radio_start(open);
        }
    } else if (open != was_open || (!open && new_key)) {
        e = radio_apply(open);   /* a change of the key disconnects the clients */
    }
    if (e != ESP_OK) {
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "commissioning access point not started: %s", esp_err_to_name(e));
        return e;
    }
    gw_aplife_start(&s_life, now_ms(), max_s * 1000u, open_s * 1000u);
    dns_follow();
    if (!was_running) {
        esp_timer_start_periodic(s_tick, (uint64_t)TICK_MS * 1000u);
        char port[8] = "", addr[32];
        if (web_port != (https ? 443u : 80u)) {
            snprintf(port, sizeof(port), ":%u", (unsigned)web_port);
        }
        /* the address of the netif, not a constant: it stays correct if the subnet of the
         * access point becomes configurable (192.168.4.1/24 is the ESP-IDF default) */
        radio_addr(addr, sizeof(addr));
#if CONFIG_GW_SOFTAP
        const char *sim = "";
#else
        const char *sim = "SIMULATED (QEMU has no radio) ";
#endif
        ESP_LOGW(TAG, "%scommissioning access point '%s' (%s): web UI at %s://%s%s/", sim, s_ssid,
                 open ? "OPEN, no key: claim window"
                 : s_key_is_pmk ? "WPA2, the web password"
                                : "WPA2, the commissioning password",
                 https ? "https" : "http", addr, port);
    } else if (open != was_open) {
        ESP_LOGW(TAG, "access point '%s' is now %s", s_ssid,
                 open ? "OPEN, no key: claim window" : s_key_is_pmk
                                                          ? "WPA2 with the web password"
                                                          : "WPA2 with the commissioning password");
    }
    if (open) {
        ESP_LOGW(TAG, "the access point stays open for %lu s", (unsigned long)open_s);
    }
    if (max_s != 0) {
        ESP_LOGI(TAG, "access point stops after %lu s without a client, and in %lu min at the "
                      "latest", (unsigned long)(s_life.idle_ms / 1000u),
                 (unsigned long)(max_s / 60u));
    } else {
        ESP_LOGI(TAG, "access point stops after %lu s without a client",
                 (unsigned long)(s_life.idle_ms / 1000u));
    }
    bool running = s_life.running;
    xSemaphoreGive(s_lock);
    notify(running, open, GW_AP_STOP_NONE);
    return ESP_OK;
}

void gw_ap_claimed(const char *key, bool key_is_pmk)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_key, sizeof(s_key), "%s", key);
    s_key_is_pmk = key_is_pmk;
    bool open = s_life.running && s_life.open;
    gw_aplife_claimed(&s_life, now_ms());
    if (open) {
        ESP_LOGW(TAG, "access point '%s': WPA2 with %s in %lu s (the claim is done)", s_ssid,
                 key_name(), (unsigned long)(gw_aplife_open_left_ms(&s_life, now_ms()) / 1000u));
    } else if (s_life.running && radio_apply(false) == ESP_OK) {
        ESP_LOGW(TAG, "access point '%s' uses WPA2 with %s", s_ssid, key_name());
    }
    xSemaphoreGive(s_lock);
}

bool gw_ap_stop(gw_ap_stop_t why)
{
    if (s_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = gw_aplife_stop(&s_life, why);
    if (ok) {
        apply_action(GW_APLIFE_STOP, why);
    }
    xSemaphoreGive(s_lock);
    if (ok) {
        notify(false, false, why);
    }
    return ok;
}

bool gw_ap_stop_soon(void)
{
    if (s_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = gw_aplife_stop_soon(&s_life, now_ms(), STOP_SOON_MS);
    xSemaphoreGive(s_lock);
    if (ok) {
        ESP_LOGW(TAG, "access point '%s' stops in a few seconds (request)", s_ssid);
    }
    return ok;
}

void gw_ap_get_status(gw_ap_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->available = true;
#if !CONFIG_GW_SOFTAP
    out->simulated = true;
#endif
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint64_t now = now_ms();
    out->running = s_life.running;
    out->open = s_life.open;
    out->key_is_pmk = s_key_is_pmk;
    memcpy(out->ssid, s_ssid, sizeof(out->ssid));
    out->clients = s_life.clients;
    uint32_t left;
    out->limited = gw_aplife_left_ms(&s_life, now, &left);
    out->left_s = (left + 999u) / 1000u;
    out->open_left_s = (gw_aplife_open_left_ms(&s_life, now) + 999u) / 1000u;
    out->idle_s = s_life.idle_ms / 1000u;
    out->last_stop = s_life.stopped;
    xSemaphoreGive(s_lock);
    if (out->ssid[0] == '\0') {
        /* #203, #209: the name is known before the first start (it comes from the MAC); the
         * dialogs of the web UI give it for the access point of the next boot */
        gw_ap_ssid(out->ssid);
    }
}

bool gw_ap_active(void)
{
    if (s_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool r = s_life.running;
    xSemaphoreGive(s_lock);
    return r;
}

#else   /* a hardware build without the radio */

esp_err_t gw_ap_init(void (*changed)(bool running, bool open, gw_ap_stop_t why))
{
    (void)changed;
    return ESP_OK;
}

esp_err_t gw_ap_start(const char *key, bool key_is_pmk, uint32_t max_s, bool https,
                      uint16_t web_port, uint32_t open_s)
{
    (void)key;
    (void)key_is_pmk;
    (void)max_s;
    (void)https;
    (void)web_port;
    (void)open_s;
    ESP_LOGW(TAG, "commissioning access point not available in this build (CONFIG_GW_SOFTAP, "
                  "needs the Wi-Fi radio): use Ethernet");
    return ESP_ERR_NOT_SUPPORTED;
}

void gw_ap_claimed(const char *key, bool key_is_pmk)
{
    (void)key;
    (void)key_is_pmk;
}

bool gw_ap_stop(gw_ap_stop_t why)
{
    (void)why;
    return false;
}

bool gw_ap_stop_soon(void)
{
    return false;
}

void gw_ap_get_status(gw_ap_status_t *out)
{
    memset(out, 0, sizeof(*out));
}

bool gw_ap_active(void)
{
    return false;
}

bool gw_ap_ip4(uint8_t ip[4], uint8_t mask[4])
{
    (void)ip;
    (void)mask;
    return false;
}

#endif
