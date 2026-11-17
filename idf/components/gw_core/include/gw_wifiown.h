/*
 * Issue #208: who uses the Wi-Fi driver, and what the driver must do when a user comes or goes.
 * Pure C (host-tested); gw_net applies the plan to esp_wifi under one lock.
 *
 * Three users:
 *   - the station of gw_net (network.wifi), for the rest of the boot;
 *   - the commissioning access point of gw_ap, while it runs;
 *   - a scan of gw_net (GET /api/wifi/scan?new=1), for the few seconds of the scan.
 * The driver runs while one or more users need it, with the interfaces that they need: STA for
 * the station and the scan, AP for the access point. The first user initialises and starts it;
 * when the last user goes, the driver stops and is deinitialised, so that its internal RAM is
 * free again (#80) and the next user finds it as at the boot.
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
} gw_wifiown_plan_t;

/** The mode for the users @p users (a mask of gw_wifi_user_t). */
gw_wifi_mode_t gw_wifiown_mode(uint8_t users);

/** The plan from the users @p before to the users @p after. Nothing to do when the two masks
 *  need the same mode. */
gw_wifiown_plan_t gw_wifiown_plan(uint8_t before, uint8_t after);

#ifdef __cplusplus
}
#endif
