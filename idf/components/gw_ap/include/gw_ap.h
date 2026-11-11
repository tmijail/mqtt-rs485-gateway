/*
 * Wi-Fi access point for commissioning (NET-5): "GW-Setup-XXXXXX", WPA2 with the device's
 * commissioning password, DHCP server, address 192.168.4.1; the web server (gw_web) listens
 * on every interface, so the same UI is served there. Issue #120: open (no key) only for the
 * time of a claim window.
 *
 * Kept apart from gw_net (Ethernet, Wi-Fi station): the only coordination is through the
 * Wi-Fi driver itself. If gw_net already runs the station, the driver goes to AP+STA and back
 * to STA when the access point stops; otherwise this module starts and stops the driver.
 * Without CONFIG_GW_SOFTAP (no radio, e.g. QEMU) gw_ap_start() logs and returns
 * ESP_ERR_NOT_SUPPORTED.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the access point (or extend its time). Needs esp_netif_init() and the default event
 * loop. @p password: 8-63 characters, kept by the caller until the restart. @p timeout_s 0 =
 * until the restart. @p https and @p web_port: the scheme and the port the web server really
 * uses (gw_web falls back to HTTP without a device certificate), only for the log line that
 * gives the web UI's address.
 * Issue #120: @p open_s != 0 = the claim window: the access point is open (no WPA2 key) for
 * @p open_s seconds, then WPA2 with @p password again for the rest of its time. 0 = WPA2 now
 * (also when an earlier call made it open). A change of the key disconnects the clients.
 */
esp_err_t gw_ap_start(const char *password, uint32_t timeout_s, bool https, uint16_t web_port,
                      uint32_t open_s);

bool gw_ap_active(void);

/** Issue #120: the IPv4 address and netmask of the running access point (network byte order);
 *  false when it does not run (or not in this build). */
bool gw_ap_ip4(uint8_t ip[4], uint8_t mask[4]);

#ifdef __cplusplus
}
#endif
