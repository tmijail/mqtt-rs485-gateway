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
#include "gw_plan.h"
#include "gw_tagdb.h"
#include "mb_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool merge_reads;         /* MB-11 block reads */
    uint16_t max_gap;         /* largest hole bridged by a block read */
    bool force_multi_write;   /* use FC15/FC16 even for single coils/registers */
    /* Offline devices are probed (first request of the cycle, a single attempt) at
     * 2, 4, 8 ... x poll_ms, up to max(poll_ms, offline_probe_max_ms). */
    uint32_t offline_probe_max_ms;
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
    gw_tag_value_t work[GW_MAX_TAGS_PER_DEVICE];
} gw_poller_t;

void gw_poller_init(gw_poller_t *p, const gw_config_t *cfg, mb_master_t *mb,
                    const gw_poller_opts_t *opts, uint32_t now_ms);

/**
 * Device whose poll is due at monotonic time @p now_ms, or -1 when none is due;
 * then @p wait_ms receives the time until the next one (UINT32_MAX if no devices).
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
 */
gw_dev_event_t gw_poller_poll(gw_poller_t *p, uint16_t dev, uint32_t now_ms);

/**
 * Write @p value (member per gw_conv_kind) to tag @p tag, read it back and store the confirmed
 * value. A register bit (MB-6) is written by read-modify-write of its register (FC03, then
 * FC06 or FC16).
 */
gw_write_status_t gw_poller_write_value(gw_poller_t *p, uint16_t tag, const gw_tag_value_t *value);

/** gw_poller_write_value() from a double (see gw_conv_from_double). */
gw_write_status_t gw_poller_write(gw_poller_t *p, uint16_t tag, double value);

/** Number of configured devices currently offline. */
uint16_t gw_poller_offline_count(const gw_poller_t *p);

#ifdef __cplusplus
}
#endif
