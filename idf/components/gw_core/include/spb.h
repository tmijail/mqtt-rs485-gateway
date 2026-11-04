/*
 * Sparkplug B payload codec (SPB-1) on top of nanopb and the unmodified Eclipse
 * Tahu sparkplug_b.proto. Metrics are streamed through callbacks, so encoding
 * and decoding need no buffer proportional to the number of metrics besides the
 * caller's arrays. nanopb types are not exposed.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sparkplug B DataType values (sparkplug_b.proto enum DataType). */
#define SPB_DT_INT8     1u
#define SPB_DT_INT16    2u
#define SPB_DT_INT32    3u
#define SPB_DT_INT64    4u
#define SPB_DT_UINT8    5u
#define SPB_DT_UINT16   6u
#define SPB_DT_UINT32   7u
#define SPB_DT_UINT64   8u
#define SPB_DT_FLOAT    9u
#define SPB_DT_DOUBLE   10u
#define SPB_DT_BOOLEAN  11u
#define SPB_DT_STRING   12u
#define SPB_DT_DATETIME 13u
#define SPB_DT_TEXT     14u

#define SPB_NAME_MAX    63   /* longest metric name the codec accepts (nanopb max_size 64) */
#define SPB_STRING_MAX  63

/** Outgoing metric. The oneof value field used on the wire follows @c datatype. */
typedef struct {
    const char *name;        /* NULL: omitted (DATA messages use the alias only) */
    uint64_t alias;
    uint32_t datatype;
    uint64_t timestamp;      /* 0: omitted */
    bool is_null;
    union {
        double num;          /* all numeric and Boolean datatypes */
        const char *str;     /* String / Text */
    } v;
} spb_metric_t;

typedef struct {
    uint64_t timestamp;
    bool has_seq;            /* false only for NDEATH */
    uint64_t seq;
    const spb_metric_t *metrics;
    size_t metric_count;
} spb_payload_t;

/** Encode @p p into @p buf. Returns the encoded length, 0 on failure (buffer too small). */
size_t spb_encode(const spb_payload_t *p, uint8_t *buf, size_t cap);

typedef enum {
    SPB_VAL_NONE = 0,
    SPB_VAL_INT,      /* int_value  (uint32 on the wire) */
    SPB_VAL_LONG,     /* long_value (uint64 on the wire) */
    SPB_VAL_FLOAT,
    SPB_VAL_DOUBLE,
    SPB_VAL_BOOL,
    SPB_VAL_STRING,
} spb_val_kind_t;

/** Incoming metric, valid only for the duration of the callback. */
typedef struct {
    const char *name;        /* NULL when absent */
    bool has_alias;
    uint64_t alias;
    bool has_datatype;
    uint32_t datatype;
    uint64_t timestamp;
    bool is_null;
    spb_val_kind_t kind;
    union {
        uint32_t i;
        uint64_t l;
        float f;
        double d;
        bool b;
        const char *s;
    } v;
} spb_rx_metric_t;

typedef struct {
    bool has_timestamp;
    uint64_t timestamp;
    bool has_seq;
    uint64_t seq;
} spb_rx_header_t;

/** Return false to abort decoding. */
typedef bool (*spb_metric_cb_t)(void *ctx, const spb_rx_metric_t *m);

/**
 * Decode a payload, invoking @p cb once per metric in wire order. With @p cb NULL the
 * payload is only validated: a command can then be decoded in full before any metric is
 * applied, since nanopb may fail half-way (for example on a name or string longer than
 * SPB_NAME_MAX / SPB_STRING_MAX) after earlier metrics went through the callback.
 */
bool spb_decode(const uint8_t *buf, size_t len, spb_rx_header_t *hdr, spb_metric_cb_t cb,
                void *ctx);

/**
 * Numeric value of an incoming metric as a double. Signed 8/16/32-bit values
 * are recovered from int_value whether or not the sender sign-extended them to
 * 32 bits; @p datatype is the metric's declared datatype (or the one known from
 * the birth certificate when the command omits it). Returns false for strings,
 * nulls and missing values.
 */
bool spb_rx_number(const spb_rx_metric_t *m, uint32_t datatype, double *out);

/** "spBv1.0/<group>/<type>/<edge>[/<device>]". Returns false if it does not fit. */
bool spb_topic(char *out, size_t cap, const char *group, const char *type, const char *edge,
               const char *device);

#ifdef __cplusplus
}
#endif
