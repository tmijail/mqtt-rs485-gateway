#include "gw_localio.h"

#include <string.h>

static uint8_t mode_mask(const gw_config_t *cfg, gw_do_safe_t mode)
{
    uint8_t m = 0;
    for (int i = 0; i < GW_LOCAL_IO_CHANNELS; i++) {
        if (cfg->local_io.do_failsafe[i] == mode) {
            m |= (uint8_t)(1u << i);
        }
    }
    return m;
}

uint8_t gw_lio_hold_mask(const gw_config_t *cfg)
{
    return mode_mask(cfg, GW_DO_SAFE_HOLD);
}

void gw_lio_init(gw_lio_t *l, const gw_config_t *cfg, uint32_t now_ms)
{
    memset(l, 0, sizeof(*l));
    l->safe_on = mode_mask(cfg, GW_DO_SAFE_ON);
    l->safe_off = mode_mask(cfg, GW_DO_SAFE_OFF);
    l->failsafe_ms = cfg->local_io.failsafe_s * 1000u;
    l->down_since_ms = now_ms;
}

uint8_t gw_lio_boot_state(const gw_lio_t *l, uint8_t held)
{
    return (uint8_t)((held & ~(l->safe_on | l->safe_off)) | l->safe_on);
}

gw_lio_fs_event_t gw_lio_failsafe(gw_lio_t *l, bool session_up, uint32_t now_ms, uint8_t *mask,
                                  uint8_t *value)
{
    *mask = *value = 0;
    if (session_up) {
        bool was = l->tripped;
        l->session = true;
        l->tripped = false;
        return was ? GW_LIO_FS_CLEAR : GW_LIO_FS_NONE;
    }
    if (l->session) {
        l->session = false;
        l->down_since_ms = now_ms;
    }
    if (l->tripped || l->failsafe_ms == 0 || now_ms - l->down_since_ms < l->failsafe_ms) {
        return GW_LIO_FS_NONE;
    }
    l->tripped = true;
    *mask = (uint8_t)(l->safe_on | l->safe_off);
    *value = l->safe_on;
    return GW_LIO_FS_TRIP;
}

void gw_lio_save_init(gw_lio_save_t *s, const gw_config_t *cfg, const uint64_t *restored,
                      uint32_t now_ms)
{
    s->period_ms = cfg->local_io.counter_save_s * 1000u;
    gw_lio_saved(s, restored, now_ms);
}

bool gw_lio_save_due(const gw_lio_save_t *s, const uint64_t *count, uint32_t now_ms, bool force)
{
    if (memcmp(s->saved, count, sizeof(s->saved)) == 0) {
        return false;
    }
    return s->period_ms != 0 && (force || now_ms - s->last_ms >= s->period_ms);
}

void gw_lio_saved(gw_lio_save_t *s, const uint64_t *count, uint32_t now_ms)
{
    memcpy(s->saved, count, sizeof(s->saved));
    s->last_ms = now_ms;
}
