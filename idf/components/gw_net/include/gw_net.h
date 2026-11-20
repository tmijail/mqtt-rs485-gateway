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

/** Requires esp_netif_init() and the default event loop. Issue #267: turns the SAR ADC entropy
 *  source off, and starts the radio as the entropy source (gw_rng.h, step 3), also when Ethernet
 *  fails (the error of Ethernet is the result). */
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

#if CONFIG_GW_WIFI
#include "gw_wifiown.h"

/**
 * Issue #208: the Wi-Fi driver for @p user (gw_wifiown.h): the first user initialises and starts
 * it; a new user changes the mode (STA, AP or APSTA). @p configure (or NULL) sets the
 * configuration of the interface of the user, after the mode and before the start. On an error
 * the driver stays as before the call. Safe from any task; not from the event loop task.
 */
esp_err_t gw_net_wifi_acquire(gw_wifi_user_t user, esp_err_t (*configure)(void *ctx), void *ctx);

/** Issue #208: @p user gives the driver back. After the last user the driver stops and is
 *  deinitialised (its internal RAM is free); issue #267: the radio of the entropy source
 *  (GW_WIFI_USER_RNG) never goes, so this does not occur after gw_net_start. Safe from any task;
 *  not from the event loop task. */
void gw_net_wifi_release(gw_wifi_user_t user);
#endif

/* ---- #208: the Wi-Fi networks near the gateway (the button "Find networks" of the web UI) */

#define GW_NET_SCAN_MAX 20     /* networks kept from one scan, strongest first */

typedef enum {
    GW_NET_SCAN_IDLE = 0,      /* no scan yet */
    GW_NET_SCAN_RUNNING,
    GW_NET_SCAN_DONE,
    GW_NET_SCAN_FAILED,
    GW_NET_SCAN_UNAVAILABLE,   /* this build has no Wi-Fi radio (and is not the emulator) */
} gw_net_scan_state_t;

typedef struct {
    char ssid[33];
    int8_t rssi;               /* dBm */
    uint8_t channel;
    const char *security;      /* "Open", "WPA2", "WPA2/WPA3", "WPA2-Enterprise", ... */
    bool supported;            /* the station can join it: open or WPA2/WPA3 personal */
} gw_net_scan_ap_t;

typedef void (*gw_net_scan_cb_t)(void *ctx, const gw_net_scan_ap_t *ap);

/**
 * Start a scan, unless one runs or the last result is younger than @p max_age_s. Works also
 * with network.wifi off (issue #267: the radio of the entropy source runs in STA mode). In the
 * emulator
 * (CONFIG_GW_IO_SIMULATED, no radio) the result is a fixed list. Safe from any task; returns
 * the state after the call.
 */
gw_net_scan_state_t gw_net_wifi_scan_start(uint32_t max_age_s);

/**
 * The last result: @p cb gets each network (strongest first, one record for each SSID, no
 * hidden network) while the result is locked, only in the state GW_NET_SCAN_DONE. @p age_s:
 * the seconds since the end of the scan; @p error: why it failed.
 */
gw_net_scan_state_t gw_net_wifi_scan_result(gw_net_scan_cb_t cb, void *ctx, uint32_t *age_s,
                                            bool *simulated, char *error, size_t error_cap);

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
