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
    char instance[2 * GW_ID_SIZE + 2];
    if (configured) {
        snprintf(instance, sizeof(instance), "%s/%s", cfg->sparkplug.group_id,
                 cfg->sparkplug.edge_node_id);
    } else {
        snprintf(instance, sizeof(instance), "gateway setup");
    }
    ESP_RETURN_ON_ERROR(mdns_instance_name_set(instance), TAG, "instance");
    mdns_txt_item_t txt[] = {{"path", "/"}, {"product", "rs485-sparkplug-gateway"}};
    ESP_RETURN_ON_ERROR(mdns_service_add(instance, "_http", "_tcp", cfg->web.port, txt,
                                         sizeof(txt) / sizeof(txt[0])),
                        TAG, "service");
    ESP_LOGI(TAG, "%s.local, _http._tcp port %u ('%s')", cfg->network.hostname, cfg->web.port,
             instance);
    return ESP_OK;
#else
    (void)configured;
    ESP_LOGI(TAG, "not in this build (CONFIG_GW_MDNS); host name %s only in DHCP",
             cfg->network.hostname);
    return ESP_OK;
#endif
}
