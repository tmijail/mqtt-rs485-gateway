/*
 * MQTT broker selection with failover (MQTT-6).
 *
 * Pure state machine, no OS calls. The brokers are an ordered list: index 0 is the primary
 * (mqtt.host), the others the backups (mqtt.backup_brokers) in their order.
 *
 * Policy:
 *   - Every connection attempt goes to the current broker.
 *   - An attempt fails if no stable Sparkplug session comes out of it: TCP/TLS failure, CONNACK
 *     refused, SUBSCRIBE refused or unanswered, a session dropped before it was stable, the
 *     primary host announcing itself offline (SPB-13). After failover_after consecutive failed
 *     attempts the next broker in the list becomes current (after the last one, the first).
 *   - A stable session resets the count: the broker that works is kept.
 *   - Return to the primary: with primary_return_ms > 0, a stable session on a backup that has
 *     lasted that long is ended in an orderly way and the primary gets one attempt; if that
 *     attempt fails the walk goes on with the first backup. With 0 (the default) a working
 *     backup is kept until it fails.
 *   - "Round failed" (NET-4, issue #57): every broker failed failover_after times in a row
 *     since the last stable session, so the path to all of them is likely broken.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_MAX_BROKERS 4   /* mqtt.host and up to 3 backups */

typedef struct {
    uint8_t count;
    uint8_t failover_after;
    uint32_t primary_return_ms;
    uint8_t current;
    uint8_t failures;           /* consecutive failed attempts on the current broker */
    uint16_t round_failures;    /* failed attempts on any broker since the last stable session */
    bool probing_primary;       /* a return to the primary: one attempt only */
    bool stable;                /* a stable session is up on the current broker */
    uint64_t stable_since_ms;
} gw_brokersel_t;

/** @p count brokers (1..GW_MAX_BROKERS), @p failover_after >= 1. Starts on the primary. */
void gw_brokersel_init(gw_brokersel_t *s, uint8_t count, uint8_t failover_after,
                       uint32_t primary_return_ms);

/** The attempt on the current broker failed. True if the next attempt goes to another broker
 *  (gw_brokersel_t.current has changed). */
bool gw_brokersel_failed(gw_brokersel_t *s);

/** The session on the current broker became stable at @p now_ms. */
void gw_brokersel_stable(gw_brokersel_t *s, uint64_t now_ms);

/** The session ended (it had become stable); the next attempt goes to the same broker. */
void gw_brokersel_session_down(gw_brokersel_t *s);

/** True when the stable session on a backup has lasted primary_return_ms: the caller ends it
 *  and calls gw_brokersel_return_to_primary(). */
bool gw_brokersel_should_return(const gw_brokersel_t *s, uint64_t now_ms);

/** The next attempt is a single try on the primary. */
void gw_brokersel_return_to_primary(gw_brokersel_t *s);

/** Milliseconds until gw_brokersel_should_return() can become true, or UINT32_MAX. */
uint32_t gw_brokersel_next_check_ms(const gw_brokersel_t *s, uint64_t now_ms);

/** Every broker failed failover_after times in a row since the last stable session. */
bool gw_brokersel_round_failed(const gw_brokersel_t *s);

#ifdef __cplusplus
}
#endif
