/*
 * Sparkplug 3.0 Primary Host Application state (SPB-13).
 *
 * The primary host publishes, retained, on spBv1.0/STATE/<host_id> a UTF-8 JSON payload
 * {"online": true|false, "timestamp": <ms since the epoch>}. The edge node subscribes to it
 * and publishes NBIRTH only once the host is online; when the host goes offline the node
 * publishes NDEATH and disconnects.
 *
 * Pure logic, no OS calls, no allocation: a strict parser for that payload and the
 * timestamp rule. A STATE message older than the newest one already accepted (smaller
 * timestamp) is stale and ignored: a retained message can arrive after a live one, and a
 * host that restarts announces itself with a newer timestamp.
 *
 * Issue #157: the host keeps one STATE timestamp for each MQTT server
 * (tck-id-operational-behavior-host-application-multi-server-timestamp), so the newest
 * timestamp is kept for each server of the list (gw_brokersel index), across the MQTT sessions
 * on that server. A STATE on one server is never compared with the timestamp of another one.
 * Whether the host is online is per session (the retained message of the new session says it
 * again).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gw_brokersel.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GW_PHOST_INVALID = 0,   /* not a Sparkplug 3.0 STATE payload: ignored */
    GW_PHOST_STALE,         /* older than the newest STATE accepted: ignored */
    GW_PHOST_ONLINE,
    GW_PHOST_OFFLINE,
} gw_phost_result_t;

typedef struct {
    bool online;            /* per session: the host said online in this session */
    uint8_t server;         /* the MQTT server of the current session (gw_brokersel index) */
    bool have_ts[GW_MAX_BROKERS];
    uint64_t last_ts[GW_MAX_BROKERS];   /* newest accepted timestamp, per server */
} gw_phost_t;

void gw_phost_init(gw_phost_t *p);

/** New MQTT session on server @p server (0..GW_MAX_BROKERS-1, clamped): the host is not known to
 *  be online until a STATE says so. */
void gw_phost_session_start(gw_phost_t *p, uint8_t server);

/** A STATE was accepted on the server of the current session. */
bool gw_phost_have_ts(const gw_phost_t *p);

/** The newest timestamp accepted on the server of the current session (0 if none). */
uint64_t gw_phost_last_ts(const gw_phost_t *p);

/** Parse {"online":bool,"timestamp":uint} (keys in any order, whitespace allowed, other keys
 *  ignored). False if the payload is not that. */
bool gw_phost_parse(const uint8_t *payload, size_t len, bool *online, uint64_t *timestamp);

/** A STATE message for the configured host arrived. */
gw_phost_result_t gw_phost_on_state(gw_phost_t *p, const uint8_t *payload, size_t len);

#ifdef __cplusplus
}
#endif
