#include "gw_netsel.h"

#include <string.h>

void gw_netsel_init(gw_netsel_t *s, uint8_t count, const uint8_t *rank, uint32_t failback_ms)
{
    memset(s, 0, sizeof(*s));
    s->count = count > GW_NETSEL_MAX_IF ? GW_NETSEL_MAX_IF : count;
    for (uint8_t i = 0; i < s->count; i++) {
        s->rank[i] = rank[i];
    }
    s->failback_ms = failback_ms;
    s->active = GW_NETSEL_NONE;
    gw_netsel_set_hold(s, GW_NETSEL_HOLD_MIN_MS, GW_NETSEL_HOLD_MAX_MS);
}

void gw_netsel_set_hold(gw_netsel_t *s, uint32_t min_ms, uint32_t max_ms)
{
    s->hold_min_ms = min_ms;
    s->hold_max_ms = max_ms < min_ms ? min_ms : max_ms;
    for (int i = 0; i < GW_NETSEL_MAX_IF; i++) {
        s->hold_ms[i] = s->hold_min_ms;
    }
}

bool gw_netsel_held(const gw_netsel_t *s, uint8_t ifc, uint64_t now_ms)
{
    return ifc < s->count && s->held_until_ms[ifc] != 0 && now_ms < s->held_until_ms[ifc];
}

/* Best-ranked usable interface; with @p stable_only, only those usable for failback_ms; with
 * @p skip_held, none held without service. */
static int best(const gw_netsel_t *s, uint64_t now_ms, bool stable_only, bool skip_held)
{
    int b = GW_NETSEL_NONE;
    for (int i = 0; i < s->count; i++) {
        if (!s->usable[i] || (stable_only && now_ms - s->usable_since_ms[i] < s->failback_ms) ||
            (skip_held && gw_netsel_held(s, (uint8_t)i, now_ms))) {
            continue;
        }
        if (b == GW_NETSEL_NONE || s->rank[i] < s->rank[b]) {
            b = i;
        }
    }
    return b;
}

static gw_netsel_change_t evaluate(gw_netsel_t *s, uint64_t now_ms)
{
    gw_netsel_change_t c = {GW_NETSEL_UNCHANGED, s->active, s->active};
    int next = s->active;
    if (s->active == GW_NETSEL_NONE || !s->usable[s->active]) {
        /* failover: the best one with service, else any usable one */
        next = best(s, now_ms, false, true);
        if (next == GW_NETSEL_NONE) {
            next = best(s, now_ms, false, false);
        }
    } else if (gw_netsel_held(s, (uint8_t)s->active, now_ms)) {
        /* usable but no path to the broker: any other with service, at once */
        int b = best(s, now_ms, false, true);
        if (b != GW_NETSEL_NONE) {
            next = b;
        }
    } else {
        int b = best(s, now_ms, true, true);
        if (b != GW_NETSEL_NONE && s->rank[b] < s->rank[s->active]) {
            next = b;
        }
    }
    if (next == s->active) {
        return c;
    }
    c.to = next;
    c.event = s->active == GW_NETSEL_NONE ? GW_NETSEL_ACTIVATED
              : next == GW_NETSEL_NONE    ? GW_NETSEL_LOST
                                          : GW_NETSEL_SWITCHED;
    s->active = next;
    return c;
}

gw_netsel_change_t gw_netsel_set(gw_netsel_t *s, uint8_t ifc, bool usable, uint64_t now_ms)
{
    if (ifc < s->count && s->usable[ifc] != usable) {
        s->usable[ifc] = usable;
        s->usable_since_ms[ifc] = now_ms;
    }
    return evaluate(s, now_ms);
}

gw_netsel_change_t gw_netsel_poll(gw_netsel_t *s, uint64_t now_ms)
{
    return evaluate(s, now_ms);
}

gw_netsel_change_t gw_netsel_unreachable(gw_netsel_t *s, uint8_t ifc, uint64_t now_ms)
{
    if (ifc < s->count) {
        s->held_until_ms[ifc] = now_ms + s->hold_ms[ifc];
        uint64_t next = (uint64_t)s->hold_ms[ifc] * 2;
        s->hold_ms[ifc] = next > s->hold_max_ms ? s->hold_max_ms : (uint32_t)next;
    }
    return evaluate(s, now_ms);
}

void gw_netsel_reachable(gw_netsel_t *s, uint8_t ifc)
{
    if (ifc < s->count) {
        s->held_until_ms[ifc] = 0;
        s->hold_ms[ifc] = s->hold_min_ms;
    }
}

uint32_t gw_netsel_next_poll_ms(const gw_netsel_t *s, uint64_t now_ms)
{
    uint64_t next = UINT32_MAX;
    if (s->active == GW_NETSEL_NONE || !s->usable[s->active]) {
        return (uint32_t)next;
    }
    for (int i = 0; i < s->count; i++) {
        if (s->usable[i] && s->rank[i] < s->rank[s->active]) {
            uint64_t due = s->usable_since_ms[i] + s->failback_ms;
            if (gw_netsel_held(s, (uint8_t)i, now_ms) && s->held_until_ms[i] > due) {
                due = s->held_until_ms[i];   /* the hold ends after the failback wait */
            }
            uint64_t left = due > now_ms ? due - now_ms : 0;
            next = left < next ? left : next;
        }
    }
    return (uint32_t)next;
}
