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
 * host that restarts announces itself with a newer timestamp. The newest timestamp is kept
 * across MQTT sessions; whether the host is online is per session (the retained message of
 * the new session says it again).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
    bool have_ts;
    uint64_t last_ts;       /* newest accepted timestamp */
} gw_phost_t;

void gw_phost_init(gw_phost_t *p);

/** New MQTT session: the host is not known to be online until a STATE says so. */
void gw_phost_session_start(gw_phost_t *p);

/** Parse {"online":bool,"timestamp":uint} (keys in any order, whitespace allowed, other keys
 *  ignored). False if the payload is not that. */
bool gw_phost_parse(const uint8_t *payload, size_t len, bool *online, uint64_t *timestamp);

/** A STATE message for the configured host arrived. */
gw_phost_result_t gw_phost_on_state(gw_phost_t *p, const uint8_t *payload, size_t len);

#ifdef __cplusplus
}
#endif
