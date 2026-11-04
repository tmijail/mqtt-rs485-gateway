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
}

/* Best-ranked usable interface; with @p stable_only, only those usable for failback_ms. */
static int best(const gw_netsel_t *s, uint64_t now_ms, bool stable_only)
{
    int b = GW_NETSEL_NONE;
    for (int i = 0; i < s->count; i++) {
        if (!s->usable[i] || (stable_only && now_ms - s->usable_since_ms[i] < s->failback_ms)) {
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
        next = best(s, now_ms, false);
    } else {
        int b = best(s, now_ms, true);
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

uint32_t gw_netsel_next_poll_ms(const gw_netsel_t *s, uint64_t now_ms)
{
    uint64_t next = UINT32_MAX;
    if (s->active == GW_NETSEL_NONE || !s->usable[s->active]) {
        return (uint32_t)next;
    }
    for (int i = 0; i < s->count; i++) {
        if (s->usable[i] && s->rank[i] < s->rank[s->active]) {
            uint64_t due = s->usable_since_ms[i] + s->failback_ms;
            uint64_t left = due > now_ms ? due - now_ms : 0;
            next = left < next ? left : next;
        }
    }
    return (uint32_t)next;
}
