#include "gw_ap.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "ap";

#if CONFIG_GW_SOFTAP
#include "esp_check.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

static esp_netif_t *s_netif;
static esp_timer_handle_t s_timer;
static bool s_active;
static bool s_own_driver;   /* started by this module (no Wi-Fi station configured) */

static void stop_ap(void *arg)
{
    (void)arg;
    wifi_mode_t mode;
    if (!s_active || esp_wifi_get_mode(&mode) != ESP_OK) {
        return;
    }
    if (mode == WIFI_MODE_APSTA) {
        esp_wifi_set_mode(WIFI_MODE_STA);   /* the station of gw_net goes on */
    } else if (s_own_driver) {
        esp_wifi_stop();
    }
    s_active = false;
    ESP_LOGW(TAG, "commissioning access point stopped (timeout)");
}

esp_err_t gw_ap_start(const char *password, uint32_t timeout_s)
{
    if (s_timer == NULL) {
        const esp_timer_create_args_t ta = {.callback = stop_ap, .name = "ap_timeout"};
        ESP_RETURN_ON_ERROR(esp_timer_create(&ta, &s_timer), TAG, "timer");
    }
    esp_timer_stop(s_timer);
    if (!s_active) {
        if (s_netif == NULL) {
            s_netif = esp_netif_create_default_wifi_ap();
            ESP_RETURN_ON_FALSE(s_netif != NULL, ESP_FAIL, TAG, "AP netif");
        }
        wifi_mode_t mode = WIFI_MODE_NULL;
        esp_err_t e = esp_wifi_get_mode(&mode);
        if (e == ESP_ERR_WIFI_NOT_INIT) {
            const wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
            ESP_RETURN_ON_ERROR(esp_wifi_init(&wc), TAG, "wifi init");
            ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
            s_own_driver = true;
            mode = WIFI_MODE_NULL;
        } else {
            ESP_RETURN_ON_ERROR(e, TAG, "wifi mode");
        }
        bool sta = mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA;
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(sta ? WIFI_MODE_APSTA : WIFI_MODE_AP), TAG, "mode");
        uint8_t mac[6];
        ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), TAG, "mac");
        wifi_config_t ap = {0};
        int n = snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "GW-Setup-%02X%02X%02X", mac[3],
                         mac[4], mac[5]);
        ap.ap.ssid_len = (uint8_t)n;
        size_t pl = strnlen(password, sizeof(ap.ap.password) - 1);
        memcpy(ap.ap.password, password, pl);
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ap.ap.channel = 1;
        ap.ap.max_connection = 4;
        ap.ap.pmf_cfg.capable = true;
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "config");
        if (s_own_driver) {
            ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
        }
        s_active = true;
        ESP_LOGW(TAG, "commissioning access point '%s' (WPA2, commissioning password): web UI at "
                      "http://192.168.4.1/",
                 (const char *)ap.ap.ssid);
    }
    if (timeout_s != 0) {
        esp_timer_start_once(s_timer, (uint64_t)timeout_s * 1000000u);
        ESP_LOGI(TAG, "access point stops in %lu min", (unsigned long)(timeout_s / 60));
    }
    return ESP_OK;
}

bool gw_ap_active(void)
{
    return s_active;
}

#else

esp_err_t gw_ap_start(const char *password, uint32_t timeout_s)
{
    (void)password;
    (void)timeout_s;
    ESP_LOGW(TAG, "commissioning access point not available in this build (CONFIG_GW_SOFTAP, "
                  "needs the Wi-Fi radio): use Ethernet");
    return ESP_ERR_NOT_SUPPORTED;
}

bool gw_ap_active(void)
{
    return false;
}

#endif
