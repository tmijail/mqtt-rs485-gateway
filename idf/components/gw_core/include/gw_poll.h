/*
 * Modbus poll engine: schedules devices (MB-10), executes their read plan,
 * decodes values, tracks online state (MB-9) and performs DCMD writes (SPB-10).
 * Owns the mb_master_t: all bus traffic goes through the task that calls these
 * functions (MB-16).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gw_config.h"
#include "gw_devstate.h"
#include "gw_mbap.h"
#include "gw_plan.h"
#include "gw_tagdb.h"
#include "mb_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool merge_reads;         /* MB-11 block reads */
    uint16_t max_gap;         /* largest hole bridged by a block read */
    bool force_multi_write;   /* use FC15/FC16 even for single coils/registers: the default of
                                 * a device without write_multiple (#197) */
    /* Offline devices are probed (first request of the cycle, a single attempt) at
     * 2, 4, 8 ... x poll_ms, up to max(poll_ms, offline_probe_max_ms). */
    uint32_t offline_probe_max_ms;
    /* MB-14: called between two requests of a cycle, so the bus owner can serve pending
     * writes (gw_poller_write_value) without waiting for the end of the cycle. May be NULL. */
    void (*between_requests)(void *ctx);
    void *ctx;
} gw_poller_opts_t;

typedef enum {
    GW_WRITE_OK = 0,
    GW_WRITE_NOT_WRITABLE,
    GW_WRITE_BAD_VALUE,       /* NaN or infinite, or out of the register type's range after
                                 inverse scaling */
    GW_WRITE_BUS_ERROR,       /* timeout, CRC/framing error or exception response */
    GW_WRITE_UNKNOWN_TAG,     /* tag index outside the configuration */
} gw_write_status_t;

typedef struct {
    const gw_config_t *cfg;
    mb_master_t *mb;
    gw_poller_opts_t opts;
    gw_plan_t plan;
    gw_devstate_t state[GW_MAX_DEVICES];
    gw_dev_stats_t stats[GW_MAX_DEVICES];   /* published to gw_tagdb after each cycle / write */
    uint32_t next_due_ms[GW_MAX_DEVICES];
    uint32_t probe_ms[GW_MAX_DEVICES];      /* current probe interval of an offline device, 0 = online */
    uint32_t slow_due_ms[GW_MAX_DEVICES];   /* MB-12: when the slow-scan blocks are due again */
    gw_tag_value_t work[GW_MAX_TAGS_PER_DEVICE];
    int polling;                            /* device whose cycle is running, or -1 */
    uint32_t scan_rate_ms;                  /* SPB-15 override of every poll_ms, 0 = none */
} gw_poller_t;

void gw_poller_init(gw_poller_t *p, const gw_config_t *cfg, mb_master_t *mb,
                    const gw_poller_opts_t *opts, uint32_t now_ms);

/**
 * Device whose poll is due at monotonic time @p now_ms, or -1 when none is due;
 * then @p wait_ms receives the time until the next one (UINT32_MAX if no devices).
 * A disabled device (#214) is never due: it gets no request on the bus.
 */
int gw_poller_next(const gw_poller_t *p, uint32_t now_ms, uint32_t *wait_ms);

/**
 * Run one poll cycle of @p dev (started at @p now_ms) and publish it to gw_tagdb.
 *
 * A device that is offline (offline_after failed cycles in a row, or never online since
 * boot) is only probed so it does not take the bus from the healthy ones: the first
 * request of its cycle gets a single attempt, without retries, and the next cycle is
 * scheduled with an exponential backoff (2, 4, 8 ... x poll_ms, capped at
 * max(poll_ms, offline_probe_max_ms)). A device that never answered is probed from its
 * first cycle on, with the backoff starting at its first failure. If the probe is answered
 * the cycle continues normally, so the device comes (back) online (DBIRTH) at most one
 * capped interval plus one cycle after it starts answering.
 *
 * Scan classes (MB-12): a cycle reads the fast-scan blocks, plus the slow-scan ones when they are
 * due (every slow_poll_ms) or the device is offline. A fast-only cycle counts for offline
 * detection like any other (it fails if a request stays unanswered), but only a cycle that
 * read every block brings a device (back) online, so its DBIRTH carries every tag. Slow blocks
 * not reached in a failed cycle stay due. A device with only slow tags is polled every
 * slow_poll_ms.
 */
gw_dev_event_t gw_poller_poll(gw_poller_t *p, uint16_t dev, uint32_t now_ms);

/**
 * Write @p value (member per gw_conv_kind) to tag @p tag, read it back and store the confirmed
 * value. May be called from the between_requests hook, also for the device being polled: the
 * confirmed value then also replaces the cycle's working copy. A register bit (MB-6) is written by read-modify-write of its register (FC03, then
 * FC06 or FC16).
 */
gw_write_status_t gw_poller_write_value(gw_poller_t *p, uint16_t tag, const gw_tag_value_t *value);

/** gw_poller_write_value() from a double (see gw_conv_from_double). */
gw_write_status_t gw_poller_write(gw_poller_t *p, uint16_t tag, double value);

/**
 * SPB-15: poll every device every @p ms instead of its poll_ms (0 = back to each poll_ms). The
 * slow scan keeps slow_poll_ms unless that is shorter than @p ms; offline probes back off from
 * the new period. A shorter period applies at once, a longer one after the poll already due.
 */
void gw_poller_set_scan_rate(gw_poller_t *p, uint32_t ms, uint32_t now_ms);

/**
 * MB-15: run a forwarded Modbus TCP transaction (called by the bus owner) and count its attempts
 * (#69): in the stats of the device with that unit id (gw_dev_stats_t.tcp, published to
 * gw_tagdb), or in the gateway's "other units" counter, plus one forwarded request.
 */
void gw_poller_forward(gw_poller_t *p, gw_mbap_txn_t *txn);

/** Number of enabled devices currently offline (#214: a disabled device does not count). */
uint16_t gw_poller_offline_count(const gw_poller_t *p);

#ifdef __cplusplus
}
#endif
