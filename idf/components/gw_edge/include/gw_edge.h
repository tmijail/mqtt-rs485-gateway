/*
 * Sparkplug task: owns the esp-mqtt client and the gw_node engine. Producers in
 * other tasks only set flags or post small events through the functions below,
 * which never block and never allocate.
 *
 * Connection management (MQTT-1..3, SPB-2, SPB-3): auto-reconnect of esp-mqtt is
 * disabled; before every attempt the next bdSeq is taken (RAM, 0 at boot, +1 for
 * each CONNECT packet: an attempt that sent none gives its value back, #161), the
 * will is replaced by the matching NDEATH and the attempt is scheduled with
 * exponential backoff and jitter.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"
#include "gw_config.h"
#include "gw_node.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Connection state for the shell and the web UI (written by the Sparkplug task). */
typedef struct {
    uint8_t broker;          /* MQTT-6: index of the current broker (0 = mqtt.host) */
    uint8_t broker_count;
    const char *host;        /* its host and port (points into the configuration) */
    uint16_t port;
    uint8_t failures;        /* consecutive failed attempts on it */
    int8_t host_state;       /* SPB-13: -1 no STATE seen, 0 offline, 1 online */
    bool waiting_host;       /* connected and subscribed, NBIRTH waits for the host */
    bool sf_enabled;         /* SPB-14 */
    bool sf_psram;
    uint32_t sf_count;       /* samples waiting to be sent */
    uint32_t sf_cap;
    uint32_t sf_dropped;     /* overwritten before they could be sent */
    uint32_t sf_sent;        /* historical messages published */
    bool spill_on;           /* SPB-14: the ring spills to the card */
    char spill_off[96];      /* why not (sd enabled), or why it failed */
    bool spill_ready;        /* #124: the boot recovery is done */
    uint32_t spill_count;    /* samples (records) on the card waiting (0 after a failure) */
    uint32_t spilled;        /* records ever moved to the card */
    uint32_t spill_dropped;  /* records overwritten on the card unsent (card full) */
    bool spill_failed;       /* the card failed: spill stopped */
    bool spill_released;     /* #139: of it, stopped because a format took the card */
    uint32_t spill_stranded; /* #134: records left on the card by the failure */
    uint32_t spill_holes;    /* invalid blocks met in the data: lost, not replayed */
    uint32_t spill_blocks;   /* blocks written since boot */
    uint32_t spill_nblocks;  /* the ring: blocks, extents, blocks per extent, KB */
    uint32_t spill_extents;
    uint32_t spill_ext_blocks;
    uint32_t spill_kb;
    uint32_t spill_waiting_blocks;
    uint32_t spill_sb_writes;    /* superblock writes, of them checkpoints */
    uint32_t spill_checkpoints;
    uint32_t spill_ck_max;       /* most records replayed between two (the duplicates bound) */
    uint32_t spill_boot_ms;      /* the boot recovery: time and blocks read */
    uint32_t spill_boot_reads;
    const char *spill_found;     /* #152: what the boot recovery found (gw_sfspill_found_text) */
    uint32_t spill_gc_pending; /* files waiting for their delete */
    uint32_t spill_gc_files; /* files deleted, in spill_gc_steps steps */
    uint32_t spill_gc_steps;
    uint32_t spill_gc_left;  /* deletes left for the next boot */
    uint32_t max_unfed_ms;   /* longest time the Sparkplug task went without feeding its
                                watchdog, since boot */
    uint32_t max_packet;     /* issue #73: broker's MQTT 5 Maximum Packet Size, 0 = none */
    uint32_t oversize;       /* messages not published because they exceed it */
    uint32_t oversize_bytes; /* size of the last one */
    char oversize_topic[GW_TOPIC_MAX];
} gw_edge_status_t;

void gw_edge_get_status(gw_edge_status_t *out);

/** @p info (and its strings) must outlive the gateway. */
esp_err_t gw_edge_start(const gw_config_t *cfg, const gw_node_info_t *info);

void gw_edge_notify_ip(bool up);
/** NET-4: the active interface changed; the MQTT session moves to the new one. */
void gw_edge_notify_route(void);
void gw_edge_notify_time_synced(void);

/** TIME-1: SNTP has synchronised the clock at least once (safe from any task, also before
 *  gw_edge_start). */
bool gw_edge_time_synced(void);
void gw_edge_notify_device(uint16_t dev);
void gw_edge_notify_io(void);
void gw_edge_notify_write_done(uint16_t tag, bool ok);

/** Orderly restart from another task (new configuration, REST API, shell, OTA): the Sparkplug
 *  task moves the RAM ring of store & forward to the microSD card (#136, bounded), publishes
 *  NDEATH and disconnects when a session is up, then restarts. Returns at once. If the Sparkplug
 *  task does not start it within 20 s, the esp_timer task restarts without those steps. */
void gw_edge_request_restart(void);

/** #136: as gw_edge_request_restart(), but the samples of the RAM ring are not moved to the
 *  microSD card first (a factory reset: the next boot runs another configuration). */
void gw_edge_request_restart_discard(void);

/** FW-1: the firmware update's state changed (gw_fw hook; any task, never blocks). */
void gw_edge_notify_firmware(void);

/** CFG-6, #171 (shell and REST API): re-issue NBIRTH and every DBIRTH (as NCMD Node
 *  Control/Rebirth, with the same bdSeq). False if no Sparkplug session is up. */
bool gw_edge_request_rebirth(void);

#if CONFIG_GW_IO_SIMULATED
/** Issue #164, emulator only: the next @p n publishes of the Sparkplug engine (births, data)
 *  return -1 without a send, while the connection stays open. */
void gw_edge_sim_fail_publishes(unsigned n);
#endif

#ifdef __cplusplus
}
#endif
