#include "gw_authlimit.h"

void gw_authlimit_init(gw_authlimit_t *l, uint8_t max_fails, uint32_t block_ms,
                       uint32_t block_max_ms)
{
    memset(l, 0, sizeof(*l));
    l->max_fails = max_fails == 0 ? 1 : max_fails;
    l->block_ms = block_ms;
    l->block_max_ms = block_max_ms < block_ms ? block_ms : block_max_ms;
}

static gw_authlimit_slot_t *find(gw_authlimit_t *l, const uint8_t addr[16])
{
    for (int i = 0; i < GW_AUTHLIMIT_SLOTS; i++) {
        if (l->slot[i].used && memcmp(l->slot[i].addr, addr, 16) == 0) {
            return &l->slot[i];
        }
    }
    return NULL;
}

/* A free slot, else the least recently seen unblocked one, else the least recently seen. */
static gw_authlimit_slot_t *take(gw_authlimit_t *l, const uint8_t addr[16], uint64_t now_ms)
{
    gw_authlimit_slot_t *best = NULL;
    for (int pass = 0; pass < 2 && best == NULL; pass++) {
        for (int i = 0; i < GW_AUTHLIMIT_SLOTS; i++) {
            gw_authlimit_slot_t *s = &l->slot[i];
            if (!s->used) {
                best = s;
                break;
            }
            if (pass == 0 && s->blocked_until_ms > now_ms) {
                continue;
            }
            if (best == NULL || s->seen_ms < best->seen_ms) {
                best = s;
            }
        }
    }
    if (best == NULL) {
        /* Never true: pass 1 takes a used slot also when it is blocked. The GCC analyzer does
           not follow the two passes (-Wanalyzer-null-argument, #230). */
        best = &l->slot[0];
    }
    memset(best, 0, sizeof(*best));
    best->used = true;
    memcpy(best->addr, addr, 16);
    best->next_block_ms = l->block_ms;
    return best;
}

uint32_t gw_authlimit_check(gw_authlimit_t *l, const uint8_t addr[16], uint64_t now_ms)
{
    gw_authlimit_slot_t *s = find(l, addr);
    if (s == NULL || s->blocked_until_ms <= now_ms) {
        return 0;
    }
    s->seen_ms = now_ms;
    l->refused_total++;
    uint64_t left = s->blocked_until_ms - now_ms;
    return left > UINT32_MAX ? UINT32_MAX : (uint32_t)left;
}

uint32_t gw_authlimit_failed(gw_authlimit_t *l, const uint8_t addr[16], uint64_t now_ms)
{
    l->failed_total++;
    gw_authlimit_slot_t *s = find(l, addr);
    if (s == NULL) {
        s = take(l, addr, now_ms);
    }
    s->seen_ms = now_ms;
    if (++s->fails < l->max_fails) {
        return 0;
    }
    uint32_t t = s->next_block_ms;
    s->blocked_until_ms = now_ms + t;
    s->fails = 0;
    uint64_t next = (uint64_t)t * 2;
    s->next_block_ms = next > l->block_max_ms ? l->block_max_ms : (uint32_t)next;
    l->blocks_total++;
    return t;
}

void gw_authlimit_succeeded(gw_authlimit_t *l, const uint8_t addr[16])
{
    gw_authlimit_slot_t *s = find(l, addr);
    if (s != NULL) {
        memset(s, 0, sizeof(*s));
    }
}

unsigned gw_authlimit_blocked(const gw_authlimit_t *l, uint64_t now_ms)
{
    unsigned n = 0;
    for (int i = 0; i < GW_AUTHLIMIT_SLOTS; i++) {
        n += l->slot[i].used && l->slot[i].blocked_until_ms > now_ms;
    }
    return n;
}

bool gw_webstall_stuck(uint32_t now_ms, uint32_t alive_ms, bool busy, uint32_t limit_ms)
{
    return !busy && (uint32_t)(now_ms - alive_ms) > limit_ms;
}
