/* Ethernet (NET-1), optional Wi-Fi station as alternative or backup (NET-4), IPv4 by
 * DHCP or static (NET-2), optional IPv6 with SLAAC and a static address (NET-7) and SNTP
 * (TIME-1).
 *
 * Link and IP changes are handled by esp_eth/esp_wifi/esp_netif; the choice of the
 * active interface (default route, MQTT session) follows gw_netsel. Callbacks run in
 * the default event loop task. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"
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

#define GW_NET_IPV6_MAX 3      /* CONFIG_LWIP_IPV6_NUM_ADDRESSES */
#define GW_NET_IPV6_STR 40

typedef struct {
    bool configured;
    bool link;                     /* Ethernet link / Wi-Fi associated */
    bool usable;                   /* link and an address */
    bool active;                   /* carries the default route and the MQTT session */
    uint8_t ipv4[4];               /* 0.0.0.0 = none */
    char ipv6[GW_NET_IPV6_MAX][GW_NET_IPV6_STR];   /* "" = none (NET-7) */
} gw_net_if_status_t;

/** Requires esp_netif_init() and the default event loop. */
esp_err_t gw_net_start(const gw_config_t *cfg, const gw_net_callbacks_t *cb);

#if CONFIG_GW_IO_SIMULATED
/** Emulator only (issue #144): stop the Ethernet driver (openeth), before the shell's
 *  'panic net-off'. QEMU's openeth model keeps its state across a panic reset: after two panics
 *  with traffic, the next boot can hang in an endless openeth interrupt. */
void gw_net_sim_stop_eth(void);
#endif

/**
 * lwIP name of the active interface ("en1", "st2", ...) for binding the MQTT socket to it.
 * Only with more than one interface configured; false otherwise (no binding: the
 * single-interface behaviour stays as before) or when no interface is active.
 */
bool gw_net_active_ifname(char *name, size_t cap);

/** Issue #120: the IPv4 address and netmask of interface @p i (network byte order); false when it
 *  is not configured or has no address. Safe from any task. */
bool gw_net_ip4_info(gw_net_if_t i, uint8_t ip[4], uint8_t mask[4]);

/** Snapshot of every interface, for diagnostics (safe from any task). */
void gw_net_get_status(gw_net_if_status_t out[GW_NET_IF_COUNT]);

/** SPB-17: the node has a Wi-Fi station (built with CONFIG_GW_WIFI and network.wifi enabled). */
bool gw_net_has_wifi(void);

/** SPB-17: RSSI (dBm) of the associated access point; false while not associated. */
bool gw_net_wifi_rssi(int8_t *rssi);

/**
 * Issue #57: what the MQTT session found over interface @p ifname (the name given by
 * gw_net_active_ifname() when its client was created). @p reachable false: every broker
 * failed over it, so it is held without service (CONFIG_GW_NET_NO_SERVICE_MIN_S, doubling up
 * to _MAX_S) and the session moves to the other interface if that one is usable. true: a
 * session is stable over it (the hold time is reset). No effect with a single interface.
 * Safe from any task.
 */
void gw_net_report_broker(const char *ifname, bool reachable);

#ifdef __cplusplus
}
#endif
