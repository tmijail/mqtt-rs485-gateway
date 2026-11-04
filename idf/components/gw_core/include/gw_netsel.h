/*
 * Network interface selection (NET-4): which interface carries the default route
 * and the MQTT session when Ethernet and Wi-Fi are both configured.
 *
 * Pure state machine, no OS calls. Inputs: "interface i is usable" (link up and
 * an address) with a timestamp, and the passage of time. Output: the active
 * interface and what changed.
 *
 * Policy:
 *   - Every interface has a rank; the lowest rank is preferred.
 *   - With nothing active, the first usable interface becomes active at once,
 *     whatever its rank (availability first).
 *   - When the active interface stops being usable, the best usable one takes
 *     over at once (failover), or nothing is active.
 *   - A better-ranked interface takes over from a usable active one only after
 *     it has stayed usable for failback_ms (failback with hysteresis, so a
 *     flapping link does not tear the MQTT session down on every flap).
 *   - Any change from one interface to another requires a new MQTT session:
 *     a TCP connection cannot migrate between interfaces.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_NETSEL_MAX_IF 2
#define GW_NETSEL_NONE   (-1)

typedef enum {
    GW_NETSEL_UNCHANGED = 0,
    GW_NETSEL_ACTIVATED,   /* nothing was active; @to is now (network up) */
    GW_NETSEL_SWITCHED,    /* @from -> @to: move the default route and reconnect MQTT */
    GW_NETSEL_LOST,        /* @from went away and nothing else is usable (network down) */
} gw_netsel_event_t;

typedef struct {
    gw_netsel_event_t event;
    int from;              /* interface index or GW_NETSEL_NONE */
    int to;
} gw_netsel_change_t;

typedef struct {
    uint8_t count;
    uint8_t rank[GW_NETSEL_MAX_IF];
    uint32_t failback_ms;
    bool usable[GW_NETSEL_MAX_IF];
    uint64_t usable_since_ms[GW_NETSEL_MAX_IF];
    int active;
} gw_netsel_t;

/** @p rank[i] is the preference of interface i (0 = most preferred). */
void gw_netsel_init(gw_netsel_t *s, uint8_t count, const uint8_t *rank, uint32_t failback_ms);

/** Interface @p ifc became usable or unusable at @p now_ms (repeats are ignored). */
gw_netsel_change_t gw_netsel_set(gw_netsel_t *s, uint8_t ifc, bool usable, uint64_t now_ms);

/** Re-evaluate at @p now_ms (failback timers). */
gw_netsel_change_t gw_netsel_poll(gw_netsel_t *s, uint64_t now_ms);

/** Milliseconds until gw_netsel_poll() can change something, or UINT32_MAX. */
uint32_t gw_netsel_next_poll_ms(const gw_netsel_t *s, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
