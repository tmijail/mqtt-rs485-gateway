/*
 * Modbus TCP gateway (MB-15): a TCP server (port 502 by default) that forwards each MBAP
 * request to the RTU slaves through the RS485 bus owner (gw_bus_forward, MB-16) and answers
 * with the slave's response. Modbus TCP has no authentication: the server is off unless the
 * configuration enables it, and "allowed_clients" limits who may connect. A connection through
 * the commissioning access point is always refused (issue #120).
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "gw_config.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the server task if cfg->modbus_tcp.enabled (no-op otherwise). */
esp_err_t gw_mbtcp_start(const gw_config_t *cfg);

#if CONFIG_GW_IO_SIMULATED
/** Emulator only (issue #120, QEMU has no radio): with @p eth_as_ap the Ethernet interface
 *  counts as the access point (shell 'claimsim ... eth'). */
void gw_mbtcp_sim_ap(bool eth_as_ap);
#endif

#ifdef __cplusplus
}
#endif
