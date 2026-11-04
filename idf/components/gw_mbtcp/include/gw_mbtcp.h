/*
 * Modbus TCP gateway (MB-15): a TCP server (port 502 by default) that forwards each MBAP
 * request to the RTU slaves through the RS485 bus owner (gw_bus_forward, MB-16) and answers
 * with the slave's response. Modbus TCP has no authentication: the server is off unless the
 * configuration enables it, and "allowed_clients" limits who may connect.
 */
#pragma once

#include "esp_err.h"
#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the server task if cfg->modbus_tcp.enabled (no-op otherwise). */
esp_err_t gw_mbtcp_start(const gw_config_t *cfg);

#ifdef __cplusplus
}
#endif
