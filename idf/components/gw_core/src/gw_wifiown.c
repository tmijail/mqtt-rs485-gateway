#include "gw_wifiown.h"

gw_wifi_mode_t gw_wifiown_mode(uint8_t users)
{
    bool sta = (users & (GW_WIFI_USER_STA | GW_WIFI_USER_SCAN)) != 0;
    bool ap = (users & GW_WIFI_USER_AP) != 0;
    return sta && ap ? GW_WIFI_MODE_APSTA : ap ? GW_WIFI_MODE_AP : sta ? GW_WIFI_MODE_STA
                                                                      : GW_WIFI_MODE_NULL;
}

gw_wifiown_plan_t gw_wifiown_plan(uint8_t before, uint8_t after)
{
    gw_wifiown_plan_t p = {0};
    gw_wifi_mode_t from = gw_wifiown_mode(before), to = gw_wifiown_mode(after);
    p.mode = to;
    if (from == to) {
        return p;
    }
    if (to == GW_WIFI_MODE_NULL) {
        /* the last user went: back to the state of the boot (no driver, no RAM of it) */
        p.stop = true;
        p.deinit = true;
        return p;
    }
    p.init = from == GW_WIFI_MODE_NULL;
    p.set_mode = true;
    p.start = from == GW_WIFI_MODE_NULL;
    return p;
}
