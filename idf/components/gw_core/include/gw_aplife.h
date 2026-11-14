/*
 * NET-5, issue #187: the life of the commissioning access point. Pure logic: the caller gives
 * the time (ms) and the number of clients, and applies the actions to the radio (gw_ap).
 *
 *   - It starts on the BOOT gesture of 2-10 s, and at the boot of the modes without a valid
 *     configuration (recovery, commissioning, safe mode, the PSRAM fault mode). A start while it
 *     runs starts its times again.
 *   - It stops on request (the web UI, POST /api/ap/stop, the shell 'ap stop'), after
 *     idle_ms without a client (all modes), and at the end of its maximum time (normal mode:
 *     CONFIG_GW_SOFTAP_TIMEOUT_MIN; 0 = no maximum).
 *   - The open period (no key, issue #120: the claim window) ends at the end of its time, or
 *     GW_APLIFE_CLAIM_SECURE_MS after a successful claim, so that the browser gets the answer of
 *     the claim before the change to WPA2 disconnects it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The time from the 200 of a claim to the change to WPA2 with the new key. */
#define GW_APLIFE_CLAIM_SECURE_MS 5000u

typedef enum {
    GW_AP_STOP_NONE = 0,   /* it never stopped since the boot */
    GW_AP_STOP_REQUEST,    /* the operator: web UI, REST API or shell */
    GW_AP_STOP_IDLE,       /* idle_ms without a client */
    GW_AP_STOP_TIME,       /* the end of its maximum time */
} gw_ap_stop_t;

typedef enum {
    GW_APLIFE_NONE = 0,
    GW_APLIFE_SECURE,      /* the open period is over: WPA2 now */
    GW_APLIFE_STOP,        /* stop the access point (the reason in *why) */
} gw_aplife_action_t;

typedef struct {
    bool running;
    bool open;               /* no key (the open period of a claim window) */
    unsigned clients;
    uint32_t idle_ms;        /* stop after this time without a client; 0 = never */
    uint64_t idle_since_ms;  /* the start, or the time when the last client left */
    uint64_t max_until_ms;   /* 0 = no maximum time */
    uint64_t open_until_ms;  /* the end of the open period (while open) */
    uint64_t stop_at_ms;     /* a stop on request at this time; 0 = none */
    gw_ap_stop_t stopped;    /* the reason of the last stop */
    uint32_t starts;         /* starts since the boot */
} gw_aplife_t;

void gw_aplife_init(gw_aplife_t *l, uint32_t idle_ms);

/**
 * Start the access point at @p now_ms, or start its times again while it runs (a new BOOT
 * gesture). @p max_ms: its maximum time (0 = none). @p open_ms: an open period of this length
 * (0 = WPA2 now, also when an earlier start made it open). The idle time counts from now.
 */
void gw_aplife_start(gw_aplife_t *l, uint64_t now_ms, uint32_t max_ms, uint32_t open_ms);

/** The number of clients now. When the last client leaves, the idle time counts from now. */
void gw_aplife_clients(gw_aplife_t *l, unsigned n, uint64_t now_ms);

/** A successful claim at @p now_ms: the open period ends GW_APLIFE_CLAIM_SECURE_MS later (or
 *  earlier, at the end of its own time). No effect when the access point is not open. */
void gw_aplife_claimed(gw_aplife_t *l, uint64_t now_ms);

/** A stop on request @p delay_ms after @p now_ms (so that the answer of the request goes out
 *  first). False when the access point does not run. */
bool gw_aplife_stop_soon(gw_aplife_t *l, uint64_t now_ms, uint32_t delay_ms);

/** Stop now. False when the access point does not run. */
bool gw_aplife_stop(gw_aplife_t *l, gw_ap_stop_t why);

/**
 * What to do at @p now_ms. GW_APLIFE_STOP has priority (the reason goes to @p why and the state
 * is "stopped"); GW_APLIFE_SECURE ends the open period. Call it about once a second.
 */
gw_aplife_action_t gw_aplife_tick(gw_aplife_t *l, uint64_t now_ms, gw_ap_stop_t *why);

/**
 * The time to the stop at @p now_ms if no client comes or goes. False when there is no limit
 * now (a client is connected and there is no maximum time, or the access point does not run).
 */
bool gw_aplife_left_ms(const gw_aplife_t *l, uint64_t now_ms, uint32_t *left_ms);

/** The time left of the open period (0 when not open). */
uint32_t gw_aplife_open_left_ms(const gw_aplife_t *l, uint64_t now_ms);

/** A short English text for @p why ("request", "no client", ...), for the log and the shell. */
const char *gw_ap_stop_text(gw_ap_stop_t why);

/** The key of @p why in the REST API: "none", "request", "idle", "time". */
const char *gw_ap_stop_key(gw_ap_stop_t why);

#ifdef __cplusplus
}
#endif
