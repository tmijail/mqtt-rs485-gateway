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
#define SPB_DT_DATASET  16u   /* SPB-19 */
#define SPB_DT_BYTES    17u   /* SPB-19 */
#define SPB_DT_FILE     18u   /* SPB-19: bytes_value, like Bytes (no metadata) */
#define SPB_DT_TEMPLATE 19u   /* SPB-18 */

#define SPB_NAME_MAX    63   /* longest metric name the codec accepts (nanopb max_size 64) */
#define SPB_STRING_MAX  63
#define SPB_BYTES_MAX   64   /* longest Bytes / File value (nanopb max_size 64) */

/* SPB-16 Quality property (Int32), as used by Sparkplug hosts. */
#define SPB_QUALITY_BAD   0
#define SPB_QUALITY_GOOD  192
#define SPB_QUALITY_STALE 500

/** Standard metric properties (SPB-16), published as the metric's PropertySet. */
typedef struct {
    const char *eng_unit;         /* engUnit (String); NULL or "": omitted */
    bool has_eng_low;
    bool has_eng_high;
    double eng_low;               /* engLow (Double) */
    double eng_high;              /* engHigh (Double) */
    bool has_quality;
    int32_t quality;              /* Quality (Int32): SPB_QUALITY_* */
} spb_props_t;

struct spb_dataset;
struct spb_template;
struct spb_metric;

/** A value; the member in use follows the datatype it goes with. */
typedef union {
    double num;              /* every numeric and Boolean datatype except the 64-bit integers */
    int64_t i64;             /* Int64: exact (a double is not above 2^53) */
    uint64_t u64;            /* UInt64 and DateTime */
    const char *str;         /* String / Text */
    struct {
        const uint8_t *data; /* Bytes / File: at most SPB_BYTES_MAX bytes */
        size_t len;
    } bytes;
    const struct spb_dataset *ds;   /* DataSet */
    const struct spb_template *tmpl;  /* Template */
} spb_value_t;

/**
 * SPB-19 outgoing DataSet: a table of @c rows x @c columns. The cells are produced on demand by
 * @c cell (the encoder may call it more than once per cell: it must be deterministic), so a
 * table needs no storage of its own. Column types: the numeric and Boolean datatypes, String
 * and Text; a String cell must stay valid until the payload is encoded.
 */
typedef struct spb_dataset {
    uint32_t columns;
    const char *const *names;        /* @c columns names */
    const uint32_t *types;           /* @c columns SPB_DT_* */
    uint32_t rows;
    void (*cell)(const void *ctx, uint32_t row, uint32_t col, spb_value_t *out);
    const void *ctx;
} spb_dataset_t;

/**
 * SPB-18 outgoing Template: a definition (NBIRTH; @c ref NULL) or an instance of the definition
 * named @c ref. Its @c count members are @c members[i], or produced on demand by @c get (which
 * may be called more than once per member: it must be deterministic). Members name themselves
 * and carry no alias (no_alias); a member is not itself a template.
 */
typedef struct spb_template {
    const char *ref;
    bool is_definition;
    uint32_t count;
    const struct spb_metric *members;
    void (*get)(const void *ctx, uint32_t i, struct spb_metric *out);
    const void *ctx;
} spb_template_t;

/** Outgoing metric. The oneof value field used on the wire follows @c datatype. */
typedef struct spb_metric {
    const char *name;        /* NULL: omitted (DATA messages use the alias only) */
    bool no_alias;           /* template definitions and members: no alias on the wire */
    uint64_t alias;
    uint32_t datatype;
    uint64_t timestamp;      /* 0: omitted */
    bool is_null;
    bool is_historical;      /* SPB-14: stored while offline, sent later */
    spb_props_t props;       /* all absent when zeroed */
    spb_value_t v;
} spb_metric_t;

typedef struct {
    uint64_t timestamp;
    bool has_seq;            /* false only for NDEATH */
    uint64_t seq;
    const spb_metric_t *metrics;
    size_t metric_count;
} spb_payload_t;

/** Encode @p p into @p buf. Returns the encoded length, 0 on failure (buffer too small, or a
 * Bytes value longer than SPB_BYTES_MAX). */
size_t spb_encode(const spb_payload_t *p, uint8_t *buf, size_t cap);

typedef enum {
    SPB_VAL_NONE = 0,
    SPB_VAL_INT,      /* int_value  (uint32 on the wire) */
    SPB_VAL_LONG,     /* long_value (uint64 on the wire) */
    SPB_VAL_FLOAT,
    SPB_VAL_DOUBLE,
    SPB_VAL_BOOL,
    SPB_VAL_STRING,
    SPB_VAL_BYTES,    /* bytes_value: Bytes or File */
    SPB_VAL_DATASET,  /* cells through spb_decoder_t.cell */
    SPB_VAL_TEMPLATE, /* members through spb_decoder_t.member */
} spb_val_kind_t;

/** An incoming scalar value (DataSet cell). */
typedef struct {
    spb_val_kind_t kind;
    union {
        uint32_t i;
        uint64_t l;
        float f;
        double d;
        bool b;
        const char *s;
    } v;
} spb_rx_value_t;

/** One incoming property (the first SPB_RX_PROPS of a metric are kept). */
#define SPB_RX_PROPS     4
#define SPB_RX_KEY_MAX   23   /* longer keys are truncated */
typedef struct {
    char key[SPB_RX_KEY_MAX + 1];
    uint32_t type;
    bool is_null;
    spb_val_kind_t kind;
    union {
        uint32_t i;
        uint64_t l;
        float f;
        double d;
        bool b;
    } v;
    char s[SPB_STRING_MAX + 1];  /* String value */
} spb_rx_prop_t;

/** Incoming metric, valid only for the duration of the callback. */
typedef struct {
    const char *name;        /* NULL when absent */
    bool has_alias;
    uint64_t alias;
    bool has_datatype;
    uint32_t datatype;
    uint64_t timestamp;
    bool is_null;
    bool is_historical;
    spb_val_kind_t kind;
    union {
        uint32_t i;
        uint64_t l;
        float f;
        double d;
        bool b;
        const char *s;
        struct {
            const uint8_t *data;
            size_t len;
        } bytes;
    } v;
    struct {                         /* SPB_VAL_DATASET */
        uint64_t num_of_columns;     /* as declared by the sender */
        uint32_t columns;            /* names received */
        uint32_t rows;
        uint32_t types[16];          /* the first 16 column types */
        uint32_t type_count;
    } ds;
    struct {                         /* SPB_VAL_TEMPLATE */
        bool has_ref;
        char ref[40];                /* template_ref (an instance's definition) */
        bool is_definition;
        uint32_t members;
    } tmpl;
    uint8_t prop_count;              /* properties the metric carried (all of them counted) */
    spb_rx_prop_t props[SPB_RX_PROPS];
} spb_rx_metric_t;

/** Property @p key of an incoming metric, or NULL. */
const spb_rx_prop_t *spb_rx_prop(const spb_rx_metric_t *m, const char *key);

typedef struct {
    bool has_timestamp;
    uint64_t timestamp;
    bool has_seq;
    uint64_t seq;
} spb_rx_header_t;

/** Return false to abort decoding. */
typedef bool (*spb_metric_cb_t)(void *ctx, const spb_rx_metric_t *m);

/** Decoder callbacks; any may be NULL. The DataSet and Template callbacks run while their
 * metric is being decoded, before @c metric is called for it. */
typedef struct {
    spb_metric_cb_t metric;
    bool (*column)(void *ctx, uint32_t col, const char *name);   /* name truncated to 63 */
    bool (*cell)(void *ctx, uint32_t row, uint32_t col, const spb_rx_value_t *v);
    /** SPB-18: each member of a Template value, before the template's own @c metric call.
     * Members of a member template are not descended into. */
    spb_metric_cb_t member;
    void *ctx;
} spb_decoder_t;

/** spb_decode() with every callback; @p d NULL only validates. */
bool spb_decode_with(const uint8_t *buf, size_t len, spb_rx_header_t *hdr, const spb_decoder_t *d);

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
