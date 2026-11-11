#include "gw_brokersel.h"

#include <string.h>

void gw_brokersel_init(gw_brokersel_t *s, uint8_t count, uint8_t failover_after,
                       uint32_t primary_return_ms)
{
    memset(s, 0, sizeof(*s));
    s->count = count == 0 ? 1 : count > GW_MAX_BROKERS ? GW_MAX_BROKERS : count;
    s->failover_after = failover_after == 0 ? 1 : failover_after;
    s->primary_return_ms = primary_return_ms;
}

static void advance(gw_brokersel_t *s)
{
    s->current = (uint8_t)((s->current + 1) % s->count);
    s->failures = 0;
}

bool gw_brokersel_failed(gw_brokersel_t *s)
{
    s->stable = false;
    if (s->round_failures < UINT16_MAX) {
        s->round_failures++;
    }
    if (s->probing_primary) {
        /* the primary still does not work: back to the walk, from the first backup */
        s->probing_primary = false;
        s->current = 0;
        advance(s);
        return s->count > 1;
    }
    if (s->count == 1) {
        if (s->failures < s->failover_after) {
            s->failures++;   /* saturates: nowhere else to go */
        }
        return false;
    }
    if (++s->failures < s->failover_after) {
        return false;
    }
    advance(s);
    return true;
}

bool gw_brokersel_next(gw_brokersel_t *s)
{
    s->stable = false;
    s->probing_primary = false;
    if (s->count == 1) {
        return false;
    }
    advance(s);
    return true;
}

void gw_brokersel_stable(gw_brokersel_t *s, uint64_t now_ms)
{
    s->stable = true;
    s->stable_since_ms = now_ms;
    s->failures = 0;
    s->round_failures = 0;
    s->probing_primary = false;
}

void gw_brokersel_session_down(gw_brokersel_t *s)
{
    s->stable = false;
}

bool gw_brokersel_should_return(const gw_brokersel_t *s, uint64_t now_ms)
{
    return s->stable && s->current != 0 && s->primary_return_ms > 0 &&
           now_ms - s->stable_since_ms >= s->primary_return_ms;
}

void gw_brokersel_return_to_primary(gw_brokersel_t *s)
{
    s->stable = false;
    s->current = 0;
    s->failures = 0;
    s->probing_primary = s->count > 1;
}

uint32_t gw_brokersel_next_check_ms(const gw_brokersel_t *s, uint64_t now_ms)
{
    if (!s->stable || s->current == 0 || s->primary_return_ms == 0) {
        return UINT32_MAX;
    }
    uint64_t due = s->stable_since_ms + s->primary_return_ms;
    return due > now_ms ? (uint32_t)(due - now_ms) : 0;
}

bool gw_brokersel_round_failed(const gw_brokersel_t *s)
{
    return s->round_failures >= (uint32_t)s->count * s->failover_after;
}
