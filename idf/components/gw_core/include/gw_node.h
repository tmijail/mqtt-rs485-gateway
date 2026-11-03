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
 *   1 bdSeq, 2 Node Control/Rebirth, 3 Node Control/Reboot,
 *   4 Node Info/Firmware, 5 Node Info/Hardware, 6 Node Info/Reset Reason,
 *   10..17 DI/1..DI/8, 20..27 DO/1..DO/8, 100 + n device tag with global index n.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gw_config.h"
#include "gw_rbe.h"
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
#define GW_ALIAS_DI_BASE   10u
#define GW_ALIAS_DO_BASE   20u
#define GW_ALIAS_TAG_BASE  100u

#define GW_NODE_IO_COUNT        8
#define GW_NODE_BIRTH_METRICS   (6 + 2 * GW_NODE_IO_COUNT)
#define GW_SPB_PAYLOAD_MAX      6144   /* > worst-case DBIRTH: 64 tags with 39-char names */
#define GW_TOPIC_MAX            160

typedef struct {
    /** Publish; returns < 0 on failure. */
    int (*publish)(void *ctx, const char *topic, const uint8_t *data, size_t len, int qos,
                   bool retain);
    /** Drive DO @p index (0-7); the implementation stores the read-back in gw_tagdb. */
    bool (*set_do)(void *ctx, uint8_t index, bool on);
    /** Hand a write to the RS485 owner; false if its queue is full. */
    bool (*request_write)(void *ctx, uint16_t tag, double value);
    /** Restart the gateway (called after NDEATH has been published). */
    void (*reboot)(void *ctx);
    void *ctx;
} gw_node_ops_t;

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
    uint64_t bdseq;                    /* bdSeq of the current session (SPB-3) */
    uint8_t seq;                       /* next seq to send (SPB-5) */
    bool dev_born[GW_MAX_DEVICES];
    gw_rbe_t tag_rbe[GW_MAX_TAGS];
    uint8_t di_reported;
    uint8_t do_reported;

    /* Scratch, statically sized (REL-2). */
    gw_tag_value_t vals[GW_MAX_TAGS_PER_DEVICE];
    spb_metric_t metrics[GW_MAX_TAGS_PER_DEVICE > GW_NODE_BIRTH_METRICS ? GW_MAX_TAGS_PER_DEVICE
                                                                        : GW_NODE_BIRTH_METRICS];
    uint8_t buf[GW_SPB_PAYLOAD_MAX];
    char topic[GW_TOPIC_MAX];

    /* Per-command state while decoding an NCMD/DCMD. */
    int cmd_dev;
    bool cmd_rebirth;
    bool cmd_reboot;
    uint8_t cmd_do_mask;
} gw_node_t;

void gw_node_init(gw_node_t *n, const gw_config_t *cfg, const gw_node_ops_t *ops,
                  const gw_node_info_t *info);

/** Build "spBv1.0/<group>/<type>/<edge>[/<device>]" for this node. */
bool gw_node_topic(const gw_node_t *n, const char *type, const char *device, char *out, size_t cap);

/** NDEATH payload for @p bdseq (used for the MQTT will and the orderly disconnect). */
size_t gw_node_death_payload(uint64_t bdseq, uint8_t *out, size_t cap);

/** MQTT session established (subscriptions already sent): publish NBIRTH and DBIRTHs. */
void gw_node_session_up(gw_node_t *n, uint64_t bdseq);

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

/** Re-publish NBIRTH and every DBIRTH with seq restarted (SPB-9). */
void gw_node_rebirth(gw_node_t *n);

#ifdef __cplusplus
}
#endif
