#include "gw_buzz.h"

#include <string.h>

static bool reached(uint32_t now, uint32_t at)
{
    return (int32_t)(now - at) >= 0;
}

void gw_buzz_init(gw_buzz_t *b, uint8_t enabled_alarms, uint32_t after_s)
{
    memset(b, 0, sizeof(*b));
    b->enabled = enabled_alarms;
    b->after_ms = after_s * 1000u;
}

gw_alarm_t gw_buzz_update(gw_buzz_t *b, bool config_error, bool mqtt_down, bool device_offline,
                          uint32_t now_ms)
{
    const bool cond[4] = {false, device_offline, mqtt_down, config_error};
    gw_alarm_t alarm = GW_ALARM_NONE;
    for (int a = GW_ALARM_DEVICE_OFFLINE; a <= GW_ALARM_CONFIG_ERROR; a++) {
        if (cond[a] && !b->cond[a]) {
            b->since[a] = now_ms;
        }
        b->cond[a] = cond[a];
        uint32_t delay = a == GW_ALARM_CONFIG_ERROR ? 0 : b->after_ms;
        if (cond[a] && (b->enabled & GW_ALARM_BIT(a)) && now_ms - b->since[a] >= delay) {
            alarm = (gw_alarm_t)a;   /* the highest wins */
        }
    }
    if (alarm != b->alarm && alarm != GW_ALARM_NONE) {
        b->burst_at = now_ms;   /* a new (or higher) alarm sounds at once */
    }
    b->alarm = alarm;
    return alarm;
}

void gw_buzz_request(gw_buzz_t *b, uint8_t beeps)
{
    b->request = (uint8_t)(beeps > GW_BUZZ_MAX_BEEPS ? GW_BUZZ_MAX_BEEPS : beeps);
}

bool gw_buzz_tone(gw_buzz_t *b, uint32_t now_ms)
{
    if (b->left == 0 && !b->tone && reached(now_ms, b->step_at)) {
        if (b->request != 0) {
            b->left = b->request;
            b->request = 0;
        } else if (b->alarm != GW_ALARM_NONE && reached(now_ms, b->burst_at)) {
            b->left = (uint8_t)b->alarm;
            b->burst_at = now_ms + GW_BUZZ_PERIOD_MS;
        }
        b->step_at = now_ms;
    }
    if (b->left != 0 && reached(now_ms, b->step_at)) {
        if (b->tone) {
            b->tone = false;
            b->left--;
            b->step_at = now_ms + GW_BUZZ_OFF_MS;
        } else {
            b->tone = true;
            b->step_at = now_ms + GW_BUZZ_ON_MS;
        }
    }
    return b->tone;
}

bool gw_buzz_idle(const gw_buzz_t *b)
{
    return b->left == 0 && b->request == 0 && !b->tone;
}

const char *gw_alarm_name(gw_alarm_t a)
{
    switch (a) {
    case GW_ALARM_DEVICE_OFFLINE: return "device offline";
    case GW_ALARM_MQTT_DOWN: return "MQTT down";
    case GW_ALARM_CONFIG_ERROR: return "configuration error";
    default: return "none";
    }
}
