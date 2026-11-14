/*
 * TIME-1 / issue #160: when the clock of the gateway is valid for Sparkplug timestamps, and the
 * time that a browser sends (POST /api/time), pure logic.
 *
 * Sparkplug requires UTC timestamps (tck-id-payloads-timestamp-in-UTC,
 * tck-id-payloads-metric-timestamp-in-UTC). Until a source sets the clock, it counts from 1970.
 * The sources, from the worst to the best: none, the RTC at boot (TIME-2), the browser (an
 * operator, POST /api/time), SNTP. The clock is valid when its source is not "none". No source
 * makes it not valid again (gw_clock_after).
 *
 * Decision of the product owner of 2026-11-14: while the clock is not valid, the gateway opens
 * no MQTT session: no CONNECT, thus no NBIRTH (the NBIRTH is the first message of a session,
 * and its timestamp is the time of the publish: tck-id-topics-nbirth-timestamp,
 * tck-id-payloads-nbirth-timestamp). No NDATA, no DDATA and no store & forward sample either
 * (gw_node_set_time_valid). There is no time limit. gw_clock_hints() tells the operator what to
 * do.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GW_CLOCK_NONE = 0,   /* nothing set the clock: it counts from 1970 */
    GW_CLOCK_RTC,        /* the PCF85063 at boot (TIME-2): valid, not synchronised */
    GW_CLOCK_BROWSER,    /* POST /api/time: valid, not synchronised */
    GW_CLOCK_SNTP,       /* synchronised (TIME-3 "Time Synced" true) */
} gw_clock_source_t;

/** The key of @p src in /api/status ("none", "rtc", "browser", "sntp"). */
const char *gw_clock_source_key(gw_clock_source_t src);

/** The clock from @p src is valid for timestamps. */
bool gw_clock_valid(gw_clock_source_t src);

/** The source after a new one sets the clock: SNTP always wins, no source goes back to none. */
gw_clock_source_t gw_clock_after(gw_clock_source_t cur, gw_clock_source_t set);

/** A UTC time in ms that the gateway accepts from a browser: inside the range of the RTC
 *  (GW_RTC_MIN_EPOCH..GW_RTC_MAX_EPOCH, 2025-2099). */
bool gw_clock_plausible_ms(int64_t utc_ms);

typedef enum {
    GW_CLOCK_SET_OK = 0,
    GW_CLOCK_SET_BAD,      /* the body is not {"utc_ms": <integer>} or the time is not plausible */
    GW_CLOCK_SET_SYNCED,   /* SNTP synchronises the clock: a browser does not change it */
} gw_clock_set_t;

/**
 * POST /api/time: parse @p json (@p len bytes), {"utc_ms": <integer ms since the Unix epoch>},
 * and decide against the current source @p cur. GW_CLOCK_SET_OK puts the time in @p utc_ms;
 * otherwise @p why (@p cap bytes) gives the reason. Uses cJSON under gw_port_json_lock().
 */
gw_clock_set_t gw_clock_parse_set(const char *json, size_t len, gw_clock_source_t cur,
                                  int64_t *utc_ms, char *why, size_t cap);

/* Issue #160: what the operator must know about the clock (the log, the shell, the web UI). */
#define GW_CLOCK_HINT_WAIT    0x1u   /* no valid clock: no MQTT session until one */
#define GW_CLOCK_HINT_BATTERY 0x2u   /* the RTC lost the time at boot (OS bit): its battery */

/** The GW_CLOCK_HINT_* bits for the clock source @p src and @p rtc_lost (the OS bit of the RTC
 *  was set at boot). The battery hint stays after the clock became valid: the next power cut
 *  loses the time again. */
unsigned gw_clock_hints(gw_clock_source_t src, bool rtc_lost);

/** The text for the operator of one GW_CLOCK_HINT_* bit ("" for any other value). */
const char *gw_clock_hint_text(unsigned hint);

#ifdef __cplusplus
}
#endif
