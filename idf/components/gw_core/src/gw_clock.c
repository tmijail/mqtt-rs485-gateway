#include "gw_clock.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "gw_config_priv.h"
#include "gw_port.h"
#include "gw_rtc.h"

const char *gw_clock_source_key(gw_clock_source_t src)
{
    switch (src) {
    case GW_CLOCK_RTC:
        return "rtc";
    case GW_CLOCK_BROWSER:
        return "browser";
    case GW_CLOCK_SNTP:
        return "sntp";
    default:
        return "none";
    }
}

bool gw_clock_valid(gw_clock_source_t src)
{
    return src == GW_CLOCK_RTC || src == GW_CLOCK_BROWSER || src == GW_CLOCK_SNTP;
}

gw_clock_source_t gw_clock_after(gw_clock_source_t cur, gw_clock_source_t set)
{
    if (cur == GW_CLOCK_SNTP || set == GW_CLOCK_NONE) {
        return cur;
    }
    return set;
}

unsigned gw_clock_hints(gw_clock_source_t src, bool rtc_lost)
{
    return (gw_clock_valid(src) ? 0u : GW_CLOCK_HINT_WAIT) | (rtc_lost ? GW_CLOCK_HINT_BATTERY : 0u);
}

const char *gw_clock_hint_text(unsigned hint)
{
    switch (hint) {
    case GW_CLOCK_HINT_WAIT:
        return "waiting for a valid clock: no MQTT connection and no NBIRTH until then. Make sure "
               "that the gateway can reach an SNTP server (ntp.servers), or use \"Set the time "
               "from the browser\" in the web UI (Status tab)";
    case GW_CLOCK_HINT_BATTERY:
        return "the RTC lost the time while the power was off (OS bit): examine the RTC battery "
               "(a rechargeable 3 V cell, for example ML1220; never a CR1220)";
    default:
        return "";
    }
}

bool gw_clock_plausible_ms(int64_t utc_ms)
{
    return utc_ms >= GW_RTC_MIN_EPOCH * 1000LL && utc_ms <= GW_RTC_MAX_EPOCH * 1000LL + 999;
}

static gw_clock_set_t parse_root(const cJSON *root, int64_t *utc_ms, char *why, size_t cap)
{
    if (!cJSON_IsObject(root) || root->child == NULL || root->child->next != NULL ||
        strcmp(root->child->string, "utc_ms") != 0) {
        snprintf(why, cap, "expected {\"utc_ms\": <milliseconds since 1970-01-01T00:00:00Z>}");
        return GW_CLOCK_SET_BAD;
    }
    const cJSON *v = root->child;
    if (!cJSON_IsNumber(v) || v->valuedouble != floor(v->valuedouble) ||
        !gw_clock_plausible_ms((int64_t)fmax(fmin(v->valuedouble, 9e15), -9e15))) {
        snprintf(why, cap, "\"utc_ms\" must be an integer time from 2025 to 2099 (ms UTC)");
        return GW_CLOCK_SET_BAD;
    }
    *utc_ms = (int64_t)v->valuedouble;
    return GW_CLOCK_SET_OK;
}

gw_clock_set_t gw_clock_parse_set(const char *json, size_t len, gw_clock_source_t cur,
                                  int64_t *utc_ms, char *why, size_t cap)
{
    gw_port_json_lock();
    gw_config_json_hooks();
    cJSON *root = cJSON_ParseWithLength(json, len);
    gw_clock_set_t r;
    if (root == NULL) {
        snprintf(why, cap, gw_config_json_alloc_failed() ? "out of memory parsing the request"
                                                         : "the request is not valid JSON");
        r = GW_CLOCK_SET_BAD;
    } else {
        r = parse_root(root, utc_ms, why, cap);
    }
    cJSON_Delete(root);
    gw_port_json_unlock();
    if (r == GW_CLOCK_SET_OK && cur == GW_CLOCK_SNTP) {
        snprintf(why, cap, "SNTP synchronises the clock: the time of the browser is not used");
        r = GW_CLOCK_SET_SYNCED;
    }
    return r;
}
