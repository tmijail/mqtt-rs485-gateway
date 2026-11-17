/* Shared fixtures for the host tests: file loading, a simulated Modbus RTU
 * slave behind an mb_transport_t, and a recorder for published MQTT messages. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gw_config.h"
#include "gw_node.h"
#include "gw_tagdb.h"
#include "mb_master.h"

/* Read a whole file into a static buffer (NUL terminated). Aborts on error. */
const char *support_read_file(const char *path, size_t *len);

/* Parse the SPEC section 6 example into a static config. Aborts on error. */
const gw_config_t *support_spec_config(void);

/* ---- simulated slave ---- */

typedef struct {
    uint8_t unit;
    bool offline;              /* never answers */
    uint8_t mute_fc;           /* never answers requests with this function code (0 = none) */
    int crc_errors;            /* corrupt the CRC of the next N responses */
    int wrong_unit;            /* answer the next N requests with another unit id */
    uint8_t exception;         /* answer every request with this exception (0 = none) */
    bool coils[256];
    bool discrete[256];
    uint16_t holding[256];
    uint16_t input[256];
    /* observation */
    unsigned requests;
    uint8_t last_fc;
    uint8_t last_write_fc;     /* last FC05/06/15/16 seen */
    uint64_t delay_total_us;
    uint64_t clock_us;
} sim_slave_t;

/* Transport whose peer is @p s; @p s must outlive the transport. */
mb_transport_t sim_transport(sim_slave_t *s);

void sim_reset(sim_slave_t *s, uint8_t unit);

/* ---- publish recorder and node ops ---- */

#define REC_MAX 64

typedef struct {
    char topic[GW_TOPIC_MAX];
    uint8_t payload[GW_SPB_PAYLOAD_MAX];
    size_t len;
    int qos;
    bool retain;
} rec_msg_t;

typedef struct {
    rec_msg_t msgs[REC_MAX];
    size_t count;
    uint8_t dout;                /* state driven by set_do */
    int writes;                  /* request_write calls */
    uint16_t last_write_tag;
    double last_write_value;     /* .value of last_write */
    gw_tag_value_t last_write;
    bool write_queue_full;
    bool room_limited;           /* write_room returns room (otherwise plenty) */
    size_t room;
    int reboots;
    bool publish_fails;          /* publish returns -1 (a broker that went away) */
    const char *too_large_topic; /* issue #76: publishes to topics containing it are "too large" */
    int too_large;               /* publishes answered GW_NODE_PUBLISH_TOO_LARGE */
    int scan_rate_calls;
    uint32_t scan_rate;
    int fw_updates;              /* FW-1: firmware_update calls */
    int boot_others;             /* issue #144 item 6: boot_other calls */
    char fw_url[SPB_STRING_MAX + 1];
    int fw_shas;                 /* #215: firmware_sha256 calls */
    bool fw_sha_set;             /* ... the last one gave a SHA-256 (not NULL) */
    uint8_t fw_sha[32];          /* ... and this one */
    bool fw_sha_at_update;       /* a SHA-256 was set when firmware_update was called */
    gw_node_diag_t diag;         /* SPB-17: what the diagnostics op reports */
} recorder_t;

/* SPB-17 defaults of the recorder: a node with Wi-Fi, not associated. */
void recorder_default_diag(void);

extern recorder_t g_rec;

/* Attach a full-size (GW_SPB_PAYLOAD_MAX) payload buffer of its own to @p n. */
void support_node_buffer(gw_node_t *n);

/* Ops that record into g_rec. set_do updates gw_tagdb like the real driver. */
gw_node_ops_t recorder_ops(void);
void recorder_clear(void);

/* Decoded view of a recorded payload. */
#define DEC_MAX 80
typedef struct {
    char name[64];
    bool has_name;
    bool has_alias;
    uint64_t alias;
        bool has_datatype;       /* births only (#159) */
        uint32_t datatype;
        bool is_null;
        bool is_historical;
        spb_val_kind_t kind;
        double num;
        uint64_t u64;            /* long_value, exact */
        char str[SPB_STRING_MAX + 1];
        uint8_t bytes[SPB_BYTES_MAX];   /* SPB-19 Bytes / File */
        size_t bytes_len;
        uint64_t timestamp;
        /* SPB-16 properties */
        uint8_t prop_count;
        bool has_quality;
        int32_t quality;
        char eng_unit[64];       /* "" when absent */
        bool has_eng_low, has_eng_high;
        double eng_low, eng_high;
        /* SPB-18 Template */
        bool has_ref;
        char ref[40];
        bool is_definition;
        uint32_t members;
} dec_metric_t;

typedef struct {
    bool has_seq;
    uint64_t seq;
    uint64_t timestamp;
    size_t count;
    dec_metric_t m[DEC_MAX];
    size_t mem_count;            /* SPB-18: members of every template, in wire order */
    dec_metric_t mem[DEC_MAX];
} decoded_t;

bool decode_payload(const uint8_t *buf, size_t len, decoded_t *out);

/* Hand-encoded command metric: unlike spb_encode() it can omit the alias and carry names or
 * strings longer than the codec accepts (SPB_NAME_MAX / SPB_STRING_MAX). */
typedef enum { RAW_BOOL, RAW_FLOAT, RAW_STRING, RAW_DOUBLE } raw_kind_t;
typedef struct {
    const char *name;   /* NULL: omitted */
    bool has_alias;
    uint64_t alias;
    bool no_datatype;   /* omit the datatype (Sparkplug: SHOULD NOT be in a command) */
    uint32_t datatype;
    raw_kind_t kind;
    double num;
    const char *str;
} raw_metric_t;

/* Protobuf Payload {timestamp, metrics} built by hand. Returns its length. */
size_t raw_payload(uint8_t *out, size_t cap, const raw_metric_t *m, size_t n);

/* Index of the metric with @p alias in @p d, or -1. */
int decoded_find_alias(const decoded_t *d, uint64_t alias);
int decoded_find_name(const decoded_t *d, const char *name);
