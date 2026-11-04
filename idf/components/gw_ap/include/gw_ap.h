/*
 * Wi-Fi access point for commissioning (NET-5): "GW-Setup-XXXXXX", WPA2 with the device's
 * commissioning password, DHCP server, address 192.168.4.1; the web server (gw_web) listens
 * on every interface, so the same UI is served there.
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
 * loop. @p password: 8-63 characters. @p timeout_s 0 = until the restart.
 */
esp_err_t gw_ap_start(const char *password, uint32_t timeout_s);

bool gw_ap_active(void);

#ifdef __cplusplus
}
#endif
