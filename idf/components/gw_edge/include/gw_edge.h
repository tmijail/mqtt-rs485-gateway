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

/** Connection state for the shell and the web UI (written by the Sparkplug task). */
typedef struct {
    uint8_t broker;          /* MQTT-6: index of the current broker (0 = mqtt.host) */
    uint8_t broker_count;
    const char *host;        /* its host and port (points into the configuration) */
    uint16_t port;
    uint8_t failures;        /* consecutive failed attempts on it */
    int8_t host_state;       /* SPB-13: -1 no STATE seen, 0 offline, 1 online */
    bool waiting_host;       /* connected and subscribed, NBIRTH waits for the host */
    bool sf_enabled;         /* SPB-14 */
    bool sf_psram;
    uint32_t sf_count;       /* samples waiting to be sent */
    uint32_t sf_cap;
    uint32_t sf_dropped;     /* overwritten before they could be sent */
    uint32_t sf_sent;        /* historical messages published */
    bool spill_on;           /* SPB-14: the ring spills to the card */
    uint32_t spill_count;    /* samples (records) on the card waiting */
    uint32_t spilled;        /* records ever moved to the card */
    uint32_t spill_dropped;  /* records deleted from the card unsent (space) */
    bool spill_failed;       /* the card failed: spill stopped */
    uint32_t max_packet;     /* issue #73: broker's MQTT 5 Maximum Packet Size, 0 = none */
    uint32_t oversize;       /* messages not published because they exceed it */
    uint32_t oversize_bytes; /* size of the last one */
    char oversize_topic[GW_TOPIC_MAX];
} gw_edge_status_t;

void gw_edge_get_status(gw_edge_status_t *out);

/** @p info (and its strings) must outlive the gateway. */
esp_err_t gw_edge_start(const gw_config_t *cfg, const gw_node_info_t *info);

void gw_edge_notify_ip(bool up);
/** NET-4: the active interface changed; the MQTT session moves to the new one. */
void gw_edge_notify_route(void);
void gw_edge_notify_time_synced(void);

/** TIME-1: SNTP has synchronised the clock at least once (safe from any task, also before
 *  gw_edge_start). */
bool gw_edge_time_synced(void);
void gw_edge_notify_device(uint16_t dev);
void gw_edge_notify_io(void);
void gw_edge_notify_write_done(uint16_t tag, bool ok);

/** Orderly restart from another task (new configuration, REST API, shell): the Sparkplug task
 *  publishes NDEATH and disconnects first when a session is up, then restarts. */
void gw_edge_request_restart(void);

/** CFG-6: re-issue NBIRTH and every DBIRTH (as NCMD Node Control/Rebirth). False if no
 *  Sparkplug session is up. */
bool gw_edge_request_rebirth(void);

#ifdef __cplusplus
}
#endif
