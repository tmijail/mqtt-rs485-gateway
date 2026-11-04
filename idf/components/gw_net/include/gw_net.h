/* Ethernet (NET-1), optional Wi-Fi station as alternative or backup (NET-4), IPv4 by
 * DHCP or static (NET-2) and SNTP (TIME-1).
 *
 * Link and IP changes are handled by esp_eth/esp_wifi/esp_netif; the choice of the
 * active interface (default route, MQTT session) follows gw_netsel. Callbacks run in
 * the default event loop task. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GW_NET_ETH = 0,
    GW_NET_WIFI,
    GW_NET_IF_COUNT,
} gw_net_if_t;

typedef struct {
    void (*ip_changed)(bool up);   /* an interface became active / none is usable any more */
    void (*route_changed)(void);   /* the active interface is now a different one than the one
                                    * the MQTT session used: start a new session over it */
    void (*time_synced)(void);     /* first and every later SNTP synchronisation */
} gw_net_callbacks_t;

typedef struct {
    bool configured;
    bool link;                     /* Ethernet link / Wi-Fi associated */
    bool usable;                   /* link and an address */
    bool active;                   /* carries the default route and the MQTT session */
    uint8_t ipv4[4];               /* 0.0.0.0 = none */
} gw_net_if_status_t;

/** Requires esp_netif_init() and the default event loop. */
esp_err_t gw_net_start(const gw_config_t *cfg, const gw_net_callbacks_t *cb);

/**
 * lwIP name of the active interface ("en1", "st2", ...) for binding the MQTT socket to it.
 * Only with more than one interface configured; false otherwise (no binding: the
 * single-interface behaviour stays as before) or when no interface is active.
 */
bool gw_net_active_ifname(char *name, size_t cap);

/** Snapshot of every interface, for diagnostics (safe from any task). */
void gw_net_get_status(gw_net_if_status_t out[GW_NET_IF_COUNT]);

#ifdef __cplusplus
}
#endif
