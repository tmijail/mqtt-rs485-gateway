/* Ethernet (NET-1), IPv4 by DHCP or static (NET-2) and SNTP (TIME-1).
 * Link and IP changes are handled by esp_eth/esp_netif; callbacks run in the
 * default event loop task. */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*ip_changed)(bool up);   /* IPv4 address acquired / lost (or link down) */
    void (*time_synced)(void);     /* first and every later SNTP synchronisation */
} gw_net_callbacks_t;

/** Requires esp_netif_init() and the default event loop. */
esp_err_t gw_net_start(const gw_config_t *cfg, const gw_net_callbacks_t *cb);

#ifdef __cplusplus
}
#endif
