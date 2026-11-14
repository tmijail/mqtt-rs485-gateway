#include "gw_aplife.h"

#include <string.h>

void gw_aplife_init(gw_aplife_t *l, uint32_t idle_ms)
{
    memset(l, 0, sizeof(*l));
    l->idle_ms = idle_ms;
}

void gw_aplife_start(gw_aplife_t *l, uint64_t now_ms, uint32_t max_ms, uint32_t open_ms)
{
    if (!l->running) {
        l->clients = 0;
        l->starts++;
    }
    l->running = true;
    l->idle_since_ms = now_ms;
    l->max_until_ms = max_ms != 0 ? now_ms + max_ms : 0;
    l->stop_at_ms = 0;
    l->open = open_ms != 0;
    l->open_until_ms = l->open ? now_ms + open_ms : 0;
}

void gw_aplife_clients(gw_aplife_t *l, unsigned n, uint64_t now_ms)
{
    if (!l->running) {
        return;
    }
    if (n == 0 && l->clients != 0) {
        l->idle_since_ms = now_ms;
    }
    l->clients = n;
}

void gw_aplife_claimed(gw_aplife_t *l, uint64_t now_ms)
{
    uint64_t end = now_ms + GW_APLIFE_CLAIM_SECURE_MS;
    if (l->running && l->open && end < l->open_until_ms) {
        l->open_until_ms = end;
    }
}

bool gw_aplife_stop_soon(gw_aplife_t *l, uint64_t now_ms, uint32_t delay_ms)
{
    if (!l->running) {
        return false;
    }
    uint64_t at = now_ms + delay_ms;
    if (l->stop_at_ms == 0 || at < l->stop_at_ms) {
        l->stop_at_ms = at != 0 ? at : 1;
    }
    return true;
}

bool gw_aplife_stop(gw_aplife_t *l, gw_ap_stop_t why)
{
    if (!l->running) {
        return false;
    }
    l->running = false;
    l->open = false;
    l->clients = 0;
    l->stop_at_ms = 0;
    l->stopped = why;
    return true;
}

gw_aplife_action_t gw_aplife_tick(gw_aplife_t *l, uint64_t now_ms, gw_ap_stop_t *why)
{
    *why = GW_AP_STOP_NONE;
    if (!l->running) {
        return GW_APLIFE_NONE;
    }
    gw_ap_stop_t stop = GW_AP_STOP_NONE;
    if (l->stop_at_ms != 0 && now_ms >= l->stop_at_ms) {
        stop = GW_AP_STOP_REQUEST;
    } else if (l->max_until_ms != 0 && now_ms >= l->max_until_ms) {
        stop = GW_AP_STOP_TIME;
    } else if (l->idle_ms != 0 && l->clients == 0 && now_ms - l->idle_since_ms >= l->idle_ms) {
        stop = GW_AP_STOP_IDLE;
    }
    if (stop != GW_AP_STOP_NONE) {
        gw_aplife_stop(l, stop);
        *why = stop;
        return GW_APLIFE_STOP;
    }
    if (l->open && now_ms >= l->open_until_ms) {
        l->open = false;
        l->open_until_ms = 0;
        return GW_APLIFE_SECURE;
    }
    return GW_APLIFE_NONE;
}

static uint32_t until(uint64_t end, uint64_t now_ms)
{
    if (end <= now_ms) {
        return 0;
    }
    uint64_t d = end - now_ms;
    return d > UINT32_MAX ? UINT32_MAX : (uint32_t)d;
}

bool gw_aplife_left_ms(const gw_aplife_t *l, uint64_t now_ms, uint32_t *left_ms)
{
    *left_ms = 0;
    if (!l->running) {
        return false;
    }
    bool limited = false;
    uint32_t left = UINT32_MAX;
    if (l->max_until_ms != 0) {
        left = until(l->max_until_ms, now_ms);
        limited = true;
    }
    if (l->idle_ms != 0 && l->clients == 0) {
        uint32_t idle = until(l->idle_since_ms + l->idle_ms, now_ms);
        left = idle < left ? idle : left;
        limited = true;
    }
    if (l->stop_at_ms != 0) {
        uint32_t req = until(l->stop_at_ms, now_ms);
        left = req < left ? req : left;
        limited = true;
    }
    *left_ms = limited ? left : 0;
    return limited;
}

uint32_t gw_aplife_open_left_ms(const gw_aplife_t *l, uint64_t now_ms)
{
    return l->running && l->open ? until(l->open_until_ms, now_ms) : 0;
}

const char *gw_ap_stop_text(gw_ap_stop_t why)
{
    switch (why) {
    case GW_AP_STOP_NONE: return "never stopped";
    case GW_AP_STOP_REQUEST: return "on request";
    case GW_AP_STOP_IDLE: return "no client";
    case GW_AP_STOP_TIME: return "end of its maximum time";
    }
    return "unknown";
}

const char *gw_ap_stop_key(gw_ap_stop_t why)
{
    switch (why) {
    case GW_AP_STOP_NONE: return "none";
    case GW_AP_STOP_REQUEST: return "request";
    case GW_AP_STOP_IDLE: return "idle";
    case GW_AP_STOP_TIME: return "time";
    }
    return "none";
}
