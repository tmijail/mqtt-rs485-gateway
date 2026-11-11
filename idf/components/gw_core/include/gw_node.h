/*
 * Sparkplug B 3.0 Edge Node engine (SPB-2..SPB-12).
 *
 * Single-threaded: every function must be called from the same task (the
 * Sparkplug task on target). It reads values from gw_tagdb, decides what to
 * publish (births, deaths, report by exception), owns seq and the alias map, and
 * turns NCMD/DCMD messages into actions through the ops callbacks. It does not
 * know about MQTT clients, queues or drivers.
 *
 * Alias map (SPB-8, unique per node, stable for a given configuration):
 *   1 bdSeq, 2 reserved (Node Control/Rebirth has no alias in the NBIRTH, as
 *   tck-id-operational-behavior-data-commands-rebirth-name-aliases requires; an NCMD with
 *   alias 2 is still accepted as a rebirth request), 3 Node Control/Reboot,
 *   4 Node Info/Firmware, 5 Node Info/Hardware, 6 Node Info/Reset Reason,
 *   7 Node Control/Scan Rate,
 *   10..17 DI/1..DI/8, 20..27 DO/1..DO/8, 30..37 Counter/1..Counter/8 (IO-4, UInt64 pulse
 *   counters of the DIs), 40 Node Control/Firmware Update (FW-1, String URL), 41 Firmware
 *   Update/State (String), 42 Firmware Update/Progress (UInt32 %), 100 + n device tag with
 *   global index n,
 *   50..54 SPB-17 diagnostics: Node Info/Uptime (UInt64 s), Free Heap and Min Free Heap
 *   (UInt32 B, internal RAM), RSSI (Int32 dBm, only with Wi-Fi), Modbus (DataSet, one row per
 *   device with the MB-13 counters), 55 Node Info/Time Synced (TIME-3, Boolean: SNTP),
 *   56 Node Info/Modbus TCP (issue #81, DataSet: per device, the attempts of the Modbus TCP
 *   requests forwarded to its unit, plus one row for units without a device; only with
 *   modbus_tcp.enabled), 57 Node Info/Core Dump (issue #132, Boolean: a core dump is in
 *   flash).
 *   60..66 and 70..74 #174 (UMH preset only, in place of the DataSets 54 and 56): Node
 *   Info/Modbus/<counter> (Cycles, Failed Cycles, OK, Timeout, CRC, Frame, Exception) and Node
 *   Info/Modbus TCP/<counter> (OK, Timeout, CRC, Frame, Exception), UInt64 sums of all devices.
 *   80..95 #173 (UMH preset only): Devices/<name>/Online (Boolean) of device 0..15, with each
 *   ':' of the name as '/'.
 *
 * SPB-18 device types: a device with a `type` is published as one Template instance metric
 * (named after the type, alias 100 + its first tag) whose members are its tags, by name; the
 * NBIRTH carries one Template definition per type (no alias). DDATA carries the instance with
 * the changed members only. Devices without a type keep one metric per tag. #174: with the UMH
 * preset, a typed device also has one metric per tag, and the NBIRTH has no definitions.
 *
 * Store & forward (SPB-14, optional): with a gw_sf_t attached, changes that happen while the
 * node is not online (no session, or the primary host offline) are evaluated with the same
 * report-by-exception rules and stored (from the first NBIRTH of the run on: before it there
 * is no reported state to compare with, and the clock may not be set); once online, after
 * NBIRTH and the DBIRTHs (which carry the current values), gw_node_flush_history() publishes
 * them, oldest first, as DDATA/NDATA whose metrics have is_historical = true and the time
 * they were sampled.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gw_config.h"
#include "gw_rbe.h"
#include "gw_sf.h"
#include "gw_tagdb.h"
#include "spb.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_ALIAS_BDSEQ     1u
#define GW_ALIAS_REBIRTH   2u
#define GW_ALIAS_REBOOT    3u
#define GW_ALIAS_FIRMWARE  4u
#define GW_ALIAS_HARDWARE  5u
#define GW_ALIAS_RESET_REASON 6u
#define GW_ALIAS_SCAN_RATE 7u

/* SPB-15 "Node Control/Scan Rate" (UInt32, ms): 0 = every device polls at its own poll_ms;
 * otherwise this period replaces every device's poll_ms (and slow_poll_ms when shorter). */
#define GW_SCAN_RATE_MIN_MS 50u
#define GW_SCAN_RATE_MAX_MS 3600000u
#define GW_ALIAS_DI_BASE   10u
#define GW_ALIAS_DO_BASE   20u
#define GW_ALIAS_COUNT_BASE 30u
#define GW_ALIAS_FW_UPDATE 40u   /* FW-1: NCMD with the image URL */
#define GW_ALIAS_FW_STATE  41u
#define GW_ALIAS_FW_PROGRESS 42u
#define GW_ALIAS_TAG_BASE  100u
#define GW_ALIAS_UPTIME    50u   /* SPB-17 */
#define GW_ALIAS_FREE_HEAP 51u
#define GW_ALIAS_MIN_HEAP  52u
#define GW_ALIAS_RSSI      53u
#define GW_ALIAS_MODBUS    54u
#define GW_ALIAS_TIME_SYNCED 55u  /* TIME-3 */
#define GW_ALIAS_MODBUS_TCP 56u   /* issue #81 */
#define GW_ALIAS_CORE_DUMP 57u    /* issue #132 */
#define GW_ALIAS_MB_FLAT_BASE 60u /* #174: + counter, UMH preset only */
#define GW_ALIAS_TCP_FLAT_BASE 70u
#define GW_MB_FLAT_COUNT 7
#define GW_TCP_FLAT_COUNT 5
#define GW_ALIAS_ONLINE_BASE 80u  /* #173: + device index, UMH preset only */

#define GW_NODE_IO_COUNT        8
#define GW_NODE_DIAG_METRICS    6   /* SPB-17, at most (RSSI only with Wi-Fi, Modbus with devices,
                                     * Modbus TCP with the gateway enabled) */
/* #174: with the UMH preset: Uptime, Free Heap, Min Free Heap, RSSI and the plain counters */
#define GW_NODE_UMH_DIAG_METRICS (4 + GW_MB_FLAT_COUNT + GW_TCP_FLAT_COUNT)
/* Node metrics of the NBIRTH, without template definitions (3 of them FW-1; the last 2: Time
 * Synced and Core Dump) and without the UMH preset (it has no definitions, and its
 * Devices/<name>/Online come from spb_payload_t.extra) */
#define GW_NODE_BIRTH_METRICS   (10 + 3 * GW_NODE_IO_COUNT + GW_NODE_DIAG_METRICS + 2)
/* #174: the same with the UMH preset, without its Devices/<name>/Online */
#define GW_NODE_UMH_BIRTH_METRICS (10 + 3 * GW_NODE_IO_COUNT + GW_NODE_UMH_DIAG_METRICS + 2)
/* Scratch metrics: a whole device (+1 template instance), or the NBIRTH with one template
 * definition per device type. */
#define GW_NODE_METRICS_MAX \
    (GW_MAX_TAGS_PER_DEVICE + 1 > GW_NODE_BIRTH_METRICS + GW_MAX_DEVICES \
         ? GW_MAX_TAGS_PER_DEVICE + 1 : GW_NODE_BIRTH_METRICS + GW_MAX_DEVICES)
/* "Firmware Update/State" text with its NUL (< SPB_STRING_MAX, #87); longer is cut with a log,
 * the whole reason is in the shell and the web. 96, not more: the node (the Sparkplug engine,
 * one allocation) stays under 21 504 B, a TLSF size class of 1 KB steps (issue #80) */
#define GW_NODE_FW_STATE_MAX    96
#define GW_SPB_PAYLOAD_MAX      16384  /* > worst-case DBIRTH (64 tags with 39-char names and
                                         * every SPB-16 property, about 10 KB, test_spb) and
                                         * NBIRTH (64 template tags and the SPB-17 DataSet of 16
                                         * devices, about 12 KB, test_node) */
#define GW_TOPIC_MAX            GW_TOPIC_SIZE   /* gw_config.h: the validation uses it */
#define GW_NODE_HIST_RECS       (2 * GW_MAX_TAGS_PER_DEVICE)   /* records per historical message */

/** ops.publish result: the broker cannot take a message this large in the current session
 *  (MQTT 5 Maximum Packet Size, issue #73). Not a connection failure. The engine also uses it
 *  for a message that does not fit its own payload buffer (issue #158). */
#define GW_NODE_PUBLISH_TOO_LARGE (-2)

/** SPB-17 node diagnostics, read when published. */
typedef struct gw_node_diag {
    uint64_t uptime_s;
    uint32_t free_heap;        /* internal RAM, bytes */
    uint32_t min_free_heap;    /* lowest since boot */
    bool has_rssi;             /* the node has Wi-Fi (the metric exists) */
    bool rssi_valid;           /* associated: rssi is meaningful (null otherwise) */
    int32_t rssi;              /* dBm */
    bool time_synced;          /* TIME-3: the clock was synchronised by SNTP */
    bool core_dump;            /* issue #132: a core dump is in flash */
} gw_node_diag_t;

typedef struct {
    /** Publish; returns < 0 on failure (GW_NODE_PUBLISH_TOO_LARGE: too large for the broker). */
    int (*publish)(void *ctx, const char *topic, const uint8_t *data, size_t len, int qos,
                   bool retain);
    /** Drive DO @p index (0-7); the implementation stores the read-back in gw_tagdb. */
    bool (*set_do)(void *ctx, uint8_t index, bool on);
    /** Hand a write to the RS485 owner; false if its queue is full. @p value carries the
     * member the tag uses (gw_conv_kind). */
    bool (*request_write)(void *ctx, uint16_t tag, const gw_tag_value_t *value);
    /** Free entries in the RS485 owner's write queue. Only this task adds entries, so the
     * room can only grow until the next request_write (a DCMD is queued whole or not at all). */
    size_t (*write_room)(void *ctx);
    /** SPB-15: poll every device every @p ms (0 = each its own poll_ms). Not persisted. */
    void (*set_scan_rate)(void *ctx, uint32_t ms);
    /** Restart the gateway (called after NDEATH has been published). */
    void (*reboot)(void *ctx);
    /** FW-1: start a firmware update from @p url (NCMD "Node Control/Firmware Update"); the
     *  result comes back through gw_node_set_firmware_status(). NULL = not supported. */
    void (*firmware_update)(void *ctx, const char *url);
    /** SPB-17: current node diagnostics (NULL: none published). */
    void (*diagnostics)(void *ctx, struct gw_node_diag *out);
    void *ctx;
} gw_node_ops_t;

/** SPB-18: what a template definition's members are produced from. */
typedef struct {
    const gw_config_t *cfg;
    uint16_t dev;                      /* first device of the type */
} gw_node_def_ctx_t;

/** Static node description published in NBIRTH ("Node Info/..."); strings must outlive the node. */
typedef struct {
    const char *firmware;       /* FW-4 */
    const char *hardware;
    const char *reset_reason;   /* REL-5: why the gateway last restarted */
} gw_node_info_t;

typedef struct {
    const gw_config_t *cfg;
    gw_node_ops_t ops;
    gw_node_info_t info;

    bool online;                       /* NBIRTH published on the current MQTT session */
    bool birth_failed;                 /* issue #164: gw_node_take_birth_failure() */
    uint64_t bdseq;                    /* bdSeq of the current session (SPB-3) */
    uint8_t seq;                       /* next seq to send (SPB-5) */
    bool dev_born[GW_MAX_DEVICES];
    /* Issue #76: the device's DBIRTH was too large for this session's broker: not rebuilt on
     * every change, only on the next session or rebirth (the limit may differ then) */
    bool dev_too_large[GW_MAX_DEVICES];
    gw_rbe_t tag_rbe[GW_MAX_TAGS];
    uint8_t di_reported;
    uint8_t do_reported;
    uint64_t count_reported[GW_NODE_IO_COUNT];   /* IO-4 */
    uint32_t scan_rate_ms;             /* SPB-15: current override, 0 = per-device poll_ms */
    /* SPB-17 */
    bool has_rssi;                     /* fixed by the NBIRTH */
    bool time_synced_reported;         /* TIME-3: last published Time Synced */
    bool core_dump_reported;           /* issue #132: last published Core Dump */
    uint64_t diag_due_ms;              /* next periodic NDATA (0: none scheduled) */
    bool diag_restart;                 /* NBIRTH sent: the period restarts at the next tick */
    uint64_t diag_hash;                /* of the Modbus counters last published */
    gw_dev_stats_t diag_stats[GW_MAX_DEVICES];   /* snapshot being published */
    uint32_t diag_online;              /* bit d: device d online, in the snapshot */
    mb_stats_t diag_tcp_other;         /* #81: Modbus TCP attempts to units without a device */
    spb_dataset_t mb_ds;
    spb_dataset_t mbtcp_ds;
    bool do_unknown_reported;          /* DOs last published as null (expander not answering) */
    uint32_t online_reported;          /* #173: bit d: Devices/<d>/Online last published true */
    char fw_state[GW_NODE_FW_STATE_MAX];   /* FW-1: "Firmware Update/State" */
    uint32_t fw_progress;                  /* FW-1: "Firmware Update/Progress" (%) */

    /* Scratch, statically sized (REL-2). */
    gw_tag_value_t vals[GW_MAX_TAGS_PER_DEVICE];
    spb_metric_t metrics[GW_NODE_METRICS_MAX];
    spb_template_t tmpl;               /* SPB-18: the instance being published */
    spb_template_t defs[GW_MAX_DEVICES];   /* SPB-18: NBIRTH definitions, one per type */
    gw_node_def_ctx_t def_ctx[GW_MAX_DEVICES];
    uint8_t *buf;                      /* payload buffer (gw_node_set_buffer) */
    size_t buf_cap;
    char topic[GW_TOPIC_MAX];

    /* Store & forward (SPB-14); sf NULL = changes while offline are coalesced (the MVP). */
    gw_sf_t *sf;
    bool born_once;                    /* an NBIRTH went out since boot: from then on, store */
    gw_sf_rec_t hist[GW_NODE_HIST_RECS];
    char hist_str[GW_MAX_TAGS_PER_DEVICE][GW_STR_MAX + 1];   /* strings of a historical message */
    uint32_t sf_discarded;             /* stored samples of devices without DBIRTH when sent */
    uint32_t sf_sent;                  /* historical messages published */

    /* Per-command state while decoding an NCMD/DCMD. */
    int cmd_dev;
    bool cmd_count_only;               /* DCMD first pass: count the writes, apply nothing */
    size_t cmd_writes;
    bool cmd_rebirth;
    bool cmd_reboot;
    uint8_t cmd_do_mask;
    bool cmd_scan_rate;                /* NCMD carried Scan Rate: confirm it with NDATA */
} gw_node_t;

void gw_node_init(gw_node_t *n, const gw_config_t *cfg, const gw_node_ops_t *ops,
                  const gw_node_info_t *info);

/**
 * The buffer payloads are encoded into (required before the first publication). GW_SPB_PAYLOAD_MAX
 * holds the largest message the configuration allows; a smaller one (no PSRAM) makes an
 * oversized birth fail like one the broker refuses: logged, and the device stays unborn.
 */
void gw_node_set_buffer(gw_node_t *n, uint8_t *buf, size_t cap);

/** Build "spBv1.0/<group>/<type>/<edge>[/<device>]" for this node. @p device is used as it is
 *  (the MQTT wildcard "+" for the DCMD subscription); the engine adds sparkplug.device_prefix
 *  (#172) to the topics of its own device messages. */
bool gw_node_topic(const gw_node_t *n, const char *type, const char *device, char *out, size_t cap);

/**
 * SPB-3 / issue #161: the bdSeq of the MQTT CONNECT packets
 * (tck-id-topics-nbirth-bdseq-increment: "start at zero and increment by one on every new MQTT
 * CONNECT packet"). It is in RAM only: 0 for the first CONNECT after each boot, then the
 * previous value + 1 (255 -> 0). The Will of a connection attempt carries its bdSeq before the
 * attempt starts, so an attempt takes the next value (gw_bdseq_take) and gives it back when it
 * is sure that it sent no CONNECT packet (gw_bdseq_not_sent: no client, or the transport did not
 * connect). An attempt that may have sent its CONNECT keeps its value.
 */
typedef struct {
    uint8_t next;    /* the bdSeq of the next attempt */
    uint8_t taken;   /* the bdSeq of the last attempt */
    bool pending;    /* the last attempt can still give its value back */
} gw_bdseq_t;

/** At boot: the first CONNECT gets 0. */
void gw_bdseq_init(gw_bdseq_t *b);

/** The bdSeq for the Will of a new connection attempt. */
uint8_t gw_bdseq_take(gw_bdseq_t *b);

/** The last attempt sent no CONNECT packet: the next attempt gets its value again. Only once
 *  per gw_bdseq_take(). */
void gw_bdseq_not_sent(gw_bdseq_t *b);

/** NDEATH payload for @p bdseq (used for the MQTT will and the orderly disconnect). */
size_t gw_node_death_payload(uint64_t bdseq, uint8_t *out, size_t cap);

/** MQTT session established (subscriptions already sent): publish NBIRTH and DBIRTHs. False
 *  if the NBIRTH could not be published (the connection dropped): the node stays offline. */
bool gw_node_session_up(gw_node_t *n, uint64_t bdseq);

/** MQTT session lost: forget births; nothing is published. */
void gw_node_session_down(gw_node_t *n);

/** Values or online state of @p dev changed in gw_tagdb: DBIRTH, DDEATH or DDATA. */
void gw_node_device_changed(gw_node_t *n, uint16_t dev);

/** Local DI/DO changed in gw_tagdb: NDATA with the changed bits. */
void gw_node_io_changed(gw_node_t *n);

/** A requested write finished; on success the confirmed value is published (SPB-10). */
void gw_node_write_done(gw_node_t *n, uint16_t tag, bool ok);

/** Incoming NCMD/DCMD (other topics are ignored). */
void gw_node_handle_message(gw_node_t *n, const char *topic, size_t topic_len,
                            const uint8_t *payload, size_t len);

/** SPB-14: store changes made while offline in @p sf (NULL: do not). */
void gw_node_set_store(gw_node_t *n, gw_sf_t *sf);

/** SPB-14: publish up to @p max_msgs messages of stored history (only while online). Returns
 *  the samples still stored. */
size_t gw_node_flush_history(gw_node_t *n, size_t max_msgs);

/** FW-1: the update's state and progress, in the next NBIRTH and, when online and changed,
 *  in an NDATA now. */
void gw_node_set_firmware_status(gw_node_t *n, const char *state, uint32_t progress);

/** Re-publish NBIRTH and every DBIRTH with seq restarted (SPB-9). False if the NBIRTH could
 *  not be published: then no DBIRTH is sent, the node goes offline (gw_node_session_down) and
 *  gw_node_take_birth_failure() reports it (issue #164). */
bool gw_node_rebirth(gw_node_t *n);

/** Issue #164: an NBIRTH could not be published (session up, NCMD or local rebirth) since the
 *  last call. The node is offline; if the MQTT connection is still open, the caller must end
 *  it, or the node stays connected with no Sparkplug session. Clears the flag. */
bool gw_node_take_birth_failure(gw_node_t *n);

/**
 * SPB-17: call periodically (monotonic ms). Every sparkplug.diagnostics_s it publishes an NDATA
 * with Uptime, Free Heap, Min Free Heap (and RSSI), plus the Modbus DataSet when a counter
 * changed. Returns the ms until it needs to run again (UINT32_MAX: never).
 */
uint32_t gw_node_diag_tick(gw_node_t *n, uint64_t now_ms);

/** TIME-3: the clock's SNTP state; an NDATA with Node Info/Time Synced when it changed. The
 *  NBIRTH takes it from the diagnostics op. */
void gw_node_set_time_synced(gw_node_t *n, bool synced);

/** Issue #132: a core dump is in flash; an NDATA with Node Info/Core Dump when it changed (the
 *  dump was erased). The NBIRTH takes it from the diagnostics op. */
void gw_node_set_core_dump(gw_node_t *n, bool present);

#ifdef __cplusplus
}
#endif
