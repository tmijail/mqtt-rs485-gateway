/*
 * Wi-Fi access point for commissioning (NET-5): "GW-Setup-XXXXXX", DHCP server, the ESP-IDF
 * default address (192.168.4.1/24; code that needs it reads it from the netif: gw_ap_ip4); the
 * web server (gw_web) listens on every interface, so the same UI is served there.
 *
 * The key (issue #187): WPA2-PSK with the PMK of the web password (64 hexadecimal characters,
 * gw_wpa.h) when a web password is set, else with the commissioning password. Open (no key) only
 * for the open period of a claim window (issue #120), which ends a few seconds after the claim.
 *
 * Its life (issue #187, gw_aplife.h): it stops on request (gw_ap_stop, gw_ap_stop_soon), after
 * CONFIG_GW_SOFTAP_IDLE_MIN without a client (in every mode), and at the end of its maximum time
 * (the caller's @p max_s: CONFIG_GW_SOFTAP_TIMEOUT_MIN in normal mode). A new BOOT gesture starts
 * it again. A timer of this module looks at it once a second while it runs.
 *
 * Kept apart from gw_net (Ethernet, Wi-Fi station): the only coordination is through the
 * Wi-Fi driver itself. If gw_net already runs the station, the driver goes to AP+STA and back
 * to STA when the access point stops; otherwise this module starts and stops the driver.
 * Without CONFIG_GW_SOFTAP: in the emulator build (CONFIG_GW_IO_SIMULATED, QEMU has no radio) a
 * SIMULATED access point runs the same life with no radio (its clients come from the shell);
 * in a hardware build gw_ap_start() logs and returns ESP_ERR_NOT_SUPPORTED.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "gw_aplife.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The longest key: a PMK of 64 hexadecimal characters (no NUL in the Wi-Fi configuration). */
#define GW_AP_KEY_SIZE 65

typedef struct {
    bool available;        /* this build has an access point (radio, or the emulator's model) */
    bool simulated;        /* the emulator's model (QEMU has no radio) */
    bool running;
    bool open;             /* no key (the open period of a claim window) */
    bool key_is_pmk;       /* the WPA2 key is the PMK of the web password (else the
                            * commissioning password); the key of the last start */
    char ssid[33];         /* also before the first start (from the MAC); "" if unknown */
    unsigned clients;
    bool limited;          /* left_s is valid (else no limit now: a client, no maximum time) */
    uint32_t left_s;       /* time to the stop if no client comes or goes */
    uint32_t open_left_s;  /* time left of the open period */
    uint32_t idle_s;       /* the time without a client that stops it */
    gw_ap_stop_t last_stop;
} gw_ap_status_t;

/**
 * Prepare the module (its lock and its timer). Call it once at boot, before any other function.
 * @p changed: the access point started, stopped or changed its key (running, open, and why it
 * stopped). From the caller's task or the esp_timer task; it must not call gw_ap_start().
 */
esp_err_t gw_ap_init(void (*changed)(bool running, bool open, gw_ap_stop_t why));

/**
 * Start the access point (or start its times again). Needs esp_netif_init() and the default
 * event loop (not in the emulator's model). @p key: the WPA2 key, an 8-63 character passphrase or
 * a PMK of 64 hexadecimal characters (copied). @p max_s: its maximum time (0 = none). @p https
 * and @p web_port: the scheme and the port the web server really uses, only for the log line
 * that gives the web UI's address. Issue #120: @p open_s != 0 = the claim window: open (no key)
 * for @p open_s seconds, then WPA2. 0 = WPA2 now (also when an earlier call made it open). A
 * change of the key disconnects the clients.
 */
esp_err_t gw_ap_start(const char *key, bool key_is_pmk, uint32_t max_s, bool https,
                      uint16_t web_port, uint32_t open_s);

/**
 * Issue #187: a claim set the web password. @p key (its PMK) becomes the WPA2 key, and the open
 * period ends GW_APLIFE_CLAIM_SECURE_MS from now, so that the browser gets the answer first.
 */
void gw_ap_claimed(const char *key, bool key_is_pmk);

/** Stop now (the shell). False when it does not run. */
bool gw_ap_stop(gw_ap_stop_t why);

/** Stop a short time from now (the REST API: the answer goes out first). False when it does not
 *  run. */
bool gw_ap_stop_soon(void);

void gw_ap_get_status(gw_ap_status_t *out);

bool gw_ap_active(void);

/** Issue #120: the IPv4 address and netmask of the running access point (network byte order);
 *  false when it does not run (or not in this build, or the emulator's model). */
bool gw_ap_ip4(uint8_t ip[4], uint8_t mask[4]);

/** The SSID of this gateway (from the MAC address of the access point); "" if not readable. */
void gw_ap_ssid(char out[33]);

#if CONFIG_GW_IO_SIMULATED
/** Emulator only: the number of clients of the simulated access point (QEMU has no radio). */
void gw_ap_sim_clients(unsigned n);

/** Emulator only: the time without a client that stops it (s; 0 = the Kconfig time). */
void gw_ap_sim_idle_s(uint32_t s);
#endif

#ifdef __cplusplus
}
#endif
