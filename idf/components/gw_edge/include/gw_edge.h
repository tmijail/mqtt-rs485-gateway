/*
 * Sparkplug task: owns the esp-mqtt client and the gw_node engine. Producers in
 * other tasks only set flags or post small events through the functions below,
 * which never block and never allocate.
 *
 * Connection management (MQTT-1..3, SPB-2, SPB-3): auto-reconnect of esp-mqtt is
 * disabled; before every attempt a new bdSeq is taken from NVS, the will is
 * replaced by the matching NDEATH and the attempt is scheduled with exponential
 * backoff and jitter.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "gw_config.h"
#include "gw_node.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @p info (and its strings) must outlive the gateway. */
esp_err_t gw_edge_start(const gw_config_t *cfg, const gw_node_info_t *info);

void gw_edge_notify_ip(bool up);
/** NET-4: the active interface changed; the MQTT session moves to the new one. */
void gw_edge_notify_route(void);
void gw_edge_notify_time_synced(void);
void gw_edge_notify_device(uint16_t dev);
void gw_edge_notify_io(void);
void gw_edge_notify_write_done(uint16_t tag, bool ok);

/** Orderly restart from another task (new configuration, REST API, shell): the Sparkplug task
 *  publishes NDEATH and disconnects first when a session is up, then restarts. */
void gw_edge_request_restart(void);

#ifdef __cplusplus
}
#endif
