/*
 * Issue #208: who uses the Wi-Fi driver, and what the driver must do when a user comes or goes.
 * Pure C (host-tested); gw_net applies the plan to esp_wifi under one lock.
 *
 * Four users:
 *   - the station of gw_net (network.wifi), for the rest of the boot;
 *   - the commissioning access point of gw_ap, while it runs;
 *   - a scan of gw_net (GET /api/wifi/scan?new=1), for the few seconds of the scan;
 *   - issue #267: the radio as the entropy source of the random number generator (gw_rng.h), from
 *     gw_net_start until the restart. This user never goes: the driver runs at each boot, in STA
 *     mode with no connection when no other user needs it, and never stops.
 * The driver runs while one or more users need it, with the interfaces that they need: STA for
 * the station, the scan and the radio of the RNG, AP for the access point. The first user
 * initialises and starts it; when the last user goes, the driver stops and is deinitialised
 * (with the user of the RNG, this does not occur).
 *
 * The station must come before the other users of the STA interface: the default handler of
 * WIFI_EVENT_STA_START (esp_wifi, wifi_default.c) starts the network interface of the station,
 * and the driver posts this event only when the STA interface starts. gw_net_start starts the
 * station first, then the radio of the RNG; a station that comes later is refused (refuse).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GW_WIFI_USER_STA = 1u,    /* the station of network.wifi */
    GW_WIFI_USER_AP = 2u,     /* the commissioning access point */
    GW_WIFI_USER_SCAN = 4u,   /* a scan of the networks near the gateway */
    GW_WIFI_USER_RNG = 8u,    /* issue #267: the radio as the entropy source, until the restart */
} gw_wifi_user_t;

/* The values of wifi_mode_t of ESP-IDF. */
typedef enum {
    GW_WIFI_MODE_NULL = 0,
    GW_WIFI_MODE_STA = 1,
    GW_WIFI_MODE_AP = 2,
    GW_WIFI_MODE_APSTA = 3,
} gw_wifi_mode_t;

/* What the driver must do, in this order: init, set_mode, (the configuration of the new
 * interface by its user), start; or stop, deinit. */
typedef struct {
    bool init;             /* esp_wifi_init() and the RAM storage */
    bool set_mode;         /* esp_wifi_set_mode(mode) */
    gw_wifi_mode_t mode;
    bool start;            /* esp_wifi_start() */
    bool stop;             /* esp_wifi_stop() */
    bool deinit;           /* esp_wifi_deinit(), after the stop */
    bool sta_start;        /* the STA interface starts: the driver posts WIFI_EVENT_STA_START */
    bool refuse;           /* the station comes when the STA interface runs: do nothing */
} gw_wifiown_plan_t;

/** The mode for the users @p users (a mask of gw_wifi_user_t). */
gw_wifi_mode_t gw_wifiown_mode(uint8_t users);

/** The plan from the users @p before to the users @p after. Nothing to do when the two masks
 *  need the same mode. refuse (and nothing else) when the station is new in @p after and the
 *  STA interface runs in @p before. */
gw_wifiown_plan_t gw_wifiown_plan(uint8_t before, uint8_t after);

#ifdef __cplusplus
}
#endif
