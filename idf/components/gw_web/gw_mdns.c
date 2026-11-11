#include <stdio.h>

#include "esp_log.h"
#include "gw_web.h"
#include "sdkconfig.h"

#if CONFIG_GW_MDNS
#include "esp_check.h"
#include "mdns.h"
#endif

static const char *TAG = "mdns";

esp_err_t gw_web_mdns_start(const gw_config_t *cfg, bool configured)
{
#if CONFIG_GW_MDNS
    if (!cfg->network.mdns) {
        ESP_LOGI(TAG, "disabled (network.mdns = false)");
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "init");
    ESP_RETURN_ON_ERROR(mdns_hostname_set(cfg->network.hostname), TAG, "hostname");
    char instance[GW_MDNS_INSTANCE_SIZE];
    if (configured) {
        /* #172: two IDs of 32 bytes give 65 bytes, more than the label limit of 63 that
         * mdns_instance_name_set() refuses: cut at a character boundary */
        gw_config_mdns_instance(cfg, instance, sizeof(instance));
    } else {
        snprintf(instance, sizeof(instance), "gateway setup");
    }
    ESP_RETURN_ON_ERROR(mdns_instance_name_set(instance), TAG, "instance");
    mdns_txt_item_t txt[] = {{"path", "/"}, {"product", "rs485-sparkplug-gateway"}};
    /* issue #64: with HTTPS the plain port only redirects: announce the HTTPS one */
    const char *svc = cfg->web.https ? "_https" : "_http";
    uint16_t port = cfg->web.https ? cfg->web.https_port : cfg->web.port;
    ESP_RETURN_ON_ERROR(mdns_service_add(instance, svc, "_tcp", port, txt,
                                         sizeof(txt) / sizeof(txt[0])),
                        TAG, "service");
    ESP_LOGI(TAG, "%s.local, %s._tcp port %u ('%s')", cfg->network.hostname, svc, port, instance);
    return ESP_OK;
#else
    (void)configured;
    ESP_LOGI(TAG, "not in this build (CONFIG_GW_MDNS); host name %s only in DHCP",
             cfg->network.hostname);
    return ESP_OK;
#endif
}
