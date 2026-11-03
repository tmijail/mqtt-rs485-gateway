/*
 * Sparkplug B 3.0 payload codec and topic helpers (SPB-1).
 *
 * Payloads are encoded/decoded with nanopb from the official Eclipse Tahu
 * sparkplug_b.proto. Metrics are streamed through callbacks, so no payload
 * sized intermediate structure exists in RAM: the only buffer is the
 * caller-provided output buffer (REL-2).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GW_SPARKPLUG_H_
#define GW_SPARKPLUG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Tahu DataType values used by the gateway. */
enum spb_datatype {
	SPB_DT_INT8 = 1,
	SPB_DT_INT16 = 2,
	SPB_DT_INT32 = 3,
	SPB_DT_INT64 = 4,
	SPB_DT_UINT8 = 5,
	SPB_DT_UINT16 = 6,
	SPB_DT_UINT32 = 7,
	SPB_DT_UINT64 = 8,
	SPB_DT_FLOAT = 9,
	SPB_DT_DOUBLE = 10,
	SPB_DT_BOOLEAN = 11,
	SPB_DT_STRING = 12,
};

#define SPB_NAME_MAX   63 /* longest metric name accepted (nanopb max_size - 1) */
#define SPB_STRING_MAX 63 /* longest string value accepted */

/** Outgoing metric. The value member used is selected by @c datatype. */
struct spb_metric {
	const char *name;   /* NULL: name omitted (DATA messages use the alias) */
	bool has_alias;
	uint64_t alias;
	bool has_datatype;  /* BIRTH messages always carry the datatype */
	uint32_t datatype;
	uint64_t timestamp; /* 0: omitted */
	bool is_null;       /* value unknown (e.g. tag not readable): no value sent */
	union {
		int64_t i;      /* INT8..INT64 */
		uint64_t u;     /* UINT8..UINT64 */
		double d;       /* FLOAT, DOUBLE */
		bool b;         /* BOOLEAN */
		const char *s;  /* STRING */
	} v;
};

/** Produces metric @p index of a payload being encoded. */
typedef int (*spb_metric_get_t)(void *ctx, size_t index, struct spb_metric *m);

struct spb_payload_out {
	uint64_t timestamp;
	bool has_seq; /* false only for NDEATH */
	uint64_t seq;
	size_t metric_count;
	spb_metric_get_t get;
	void *ctx;
};

/**
 * @retval 0 on success, -ENOMEM if the payload does not fit in @p size,
 *         -EINVAL if a metric is invalid (name or string too long).
 */
int spb_encode(const struct spb_payload_out *p, uint8_t *buf, size_t size, size_t *len);

enum spb_value_kind {
	SPB_VAL_NONE,
	SPB_VAL_INT,    /* int_value (uint32 on the wire) */
	SPB_VAL_LONG,   /* long_value (uint64 on the wire) */
	SPB_VAL_FLOAT,
	SPB_VAL_DOUBLE,
	SPB_VAL_BOOL,
	SPB_VAL_STRING,
};

/** Metric decoded from an incoming NCMD/DCMD payload. */
struct spb_metric_in {
	bool has_name;
	char name[SPB_NAME_MAX + 1];
	bool has_alias;
	uint64_t alias;
	bool has_datatype;
	uint32_t datatype;
	bool is_null;
	enum spb_value_kind kind;
	union {
		uint32_t i;
		uint64_t l;
		float f;
		double d;
		bool b;
		char s[SPB_STRING_MAX + 1];
	} v;
};

typedef void (*spb_metric_cb_t)(void *ctx, const struct spb_metric_in *m);

/**
 * Decode a payload and call @p cb once per metric, in order.
 *
 * @retval 0 on success, -EINVAL if the payload is malformed (metrics before
 *         the error may already have been reported).
 */
int spb_decode(const uint8_t *buf, size_t len, spb_metric_cb_t cb, void *ctx);

/**
 * Numeric value of a decoded metric. Integer wire values are interpreted
 * with @p datatype (so a negative Int16 sent as a 32-bit two's complement
 * int_value is recovered); booleans map to 0/1.
 *
 * @retval 0 on success, -EINVAL if the metric has no numeric value.
 */
int spb_metric_in_number(const struct spb_metric_in *m, uint32_t datatype, double *out);

/* ---- topics ----------------------------------------------------------- */

#define SPB_NAMESPACE "spBv1.0"

/** spBv1.0/{group}/{type}/{node}[/{device}]; @p device may be NULL. */
int spb_topic(char *buf, size_t size, const char *group, const char *type, const char *node,
	      const char *device);

enum spb_cmd_target {
	SPB_CMD_NODE = 1,
	SPB_CMD_DEVICE = 2,
};

/**
 * Parse an incoming command topic addressed to this edge node.
 *
 * @return SPB_CMD_NODE for NCMD, SPB_CMD_DEVICE for DCMD (device id copied
 *         into @p device), -EINVAL for anything else.
 */
int spb_topic_parse_cmd(const char *topic, size_t len, const char *group, const char *node,
			char *device, size_t device_size);

#endif /* GW_SPARKPLUG_H_ */
