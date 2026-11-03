/* Shared fixtures for the host tests: file loading, a simulated Modbus RTU
 * slave behind an mb_transport_t, and a recorder for published MQTT messages. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gw_config.h"
#include "gw_node.h"
#include "mb_master.h"

/* Read a whole file into a static buffer (NUL terminated). Aborts on error. */
const char *support_read_file(const char *path, size_t *len);

/* Parse the SPEC section 6 example into a static config. Aborts on error. */
const gw_config_t *support_spec_config(void);

/* ---- simulated slave ---- */

typedef struct {
    uint8_t unit;
    bool offline;              /* never answers */
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
    double last_write_value;
    bool write_queue_full;
    int reboots;
} recorder_t;

extern recorder_t g_rec;

/* Ops that record into g_rec. set_do updates gw_tagdb like the real driver. */
gw_node_ops_t recorder_ops(void);
void recorder_clear(void);

/* Decoded view of a recorded payload. */
#define DEC_MAX 80
typedef struct {
    bool has_seq;
    uint64_t seq;
    uint64_t timestamp;
    size_t count;
    struct {
        char name[64];
        bool has_name;
        uint64_t alias;
        uint32_t datatype;
        bool is_null;
        spb_val_kind_t kind;
        double num;
        char str[64];
        uint64_t timestamp;
    } m[DEC_MAX];
} decoded_t;

bool decode_payload(const uint8_t *buf, size_t len, decoded_t *out);

/* Index of the metric with @p alias in @p d, or -1. */
int decoded_find_alias(const decoded_t *d, uint64_t alias);
int decoded_find_name(const decoded_t *d, const char *name);
