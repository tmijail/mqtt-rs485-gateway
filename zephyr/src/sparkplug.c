/*
 * Sparkplug B payload codec (nanopb) and topic helpers.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sparkplug.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include "proto/sparkplug_b.pb.h"

typedef org_eclipse_tahu_protobuf_Payload pb_payload_t;
typedef org_eclipse_tahu_protobuf_Payload_Metric pb_metric_t;

#define PB_PAYLOAD_FIELDS org_eclipse_tahu_protobuf_Payload_fields
#define PB_METRIC_FIELDS  org_eclipse_tahu_protobuf_Payload_Metric_fields
#define PB_TAG(name)      org_eclipse_tahu_protobuf_Payload_Metric_##name##_tag

BUILD_ASSERT(sizeof(((pb_metric_t *)0)->name) == SPB_NAME_MAX + 1, "name size mismatch");
BUILD_ASSERT(sizeof(((pb_metric_t *)0)->value.string_value) == SPB_STRING_MAX + 1,
	     "string size mismatch");

/* ---- encoding ---------------------------------------------------------- */

static int copy_str(char *dst, size_t size, const char *src)
{
	size_t n = strlen(src);

	if (n >= size) {
		return -EINVAL;
	}
	memcpy(dst, src, n + 1);
	return 0;
}

static int fill_value(pb_metric_t *pm, const struct spb_metric *m)
{
	switch (m->datatype) {
	case SPB_DT_INT8:
	case SPB_DT_INT16:
	case SPB_DT_INT32:
		/* Signed values travel as their 32-bit two's complement. */
		pm->which_value = PB_TAG(int_value);
		pm->value.int_value = (uint32_t)(int32_t)m->v.i;
		return 0;
	case SPB_DT_UINT8:
	case SPB_DT_UINT16:
	case SPB_DT_UINT32:
		pm->which_value = PB_TAG(int_value);
		pm->value.int_value = (uint32_t)m->v.u;
		return 0;
	case SPB_DT_INT64:
		pm->which_value = PB_TAG(long_value);
		pm->value.long_value = (uint64_t)m->v.i;
		return 0;
	case SPB_DT_UINT64:
		pm->which_value = PB_TAG(long_value);
		pm->value.long_value = m->v.u;
		return 0;
	case SPB_DT_FLOAT:
		pm->which_value = PB_TAG(float_value);
		pm->value.float_value = (float)m->v.d;
		return 0;
	case SPB_DT_DOUBLE:
		pm->which_value = PB_TAG(double_value);
		pm->value.double_value = m->v.d;
		return 0;
	case SPB_DT_BOOLEAN:
		pm->which_value = PB_TAG(boolean_value);
		pm->value.boolean_value = m->v.b;
		return 0;
	case SPB_DT_STRING:
		pm->which_value = PB_TAG(string_value);
		return copy_str(pm->value.string_value, sizeof(pm->value.string_value), m->v.s);
	default:
		return -EINVAL;
	}
}

struct enc_state {
	const struct spb_payload_out *p;
	int err;
};

static bool encode_metrics(pb_ostream_t *stream, const pb_field_t *field, void *const *arg)
{
	struct enc_state *st = *arg;
	const struct spb_payload_out *p = st->p;

	for (size_t i = 0; i < p->metric_count; i++) {
		pb_metric_t pm = org_eclipse_tahu_protobuf_Payload_Metric_init_zero;
		struct spb_metric m = {0};

		st->err = p->get(p->ctx, i, &m);
		if (st->err == 0 && m.name != NULL) {
			pm.has_name = true;
			st->err = copy_str(pm.name, sizeof(pm.name), m.name);
		}
		if (st->err == 0 && !m.is_null) {
			st->err = fill_value(&pm, &m);
		}
		if (st->err != 0) {
			return false;
		}
		pm.has_alias = m.has_alias;
		pm.alias = m.alias;
		pm.has_datatype = m.has_datatype;
		pm.datatype = m.datatype;
		pm.has_timestamp = m.timestamp != 0;
		pm.timestamp = m.timestamp;
		if (m.is_null) {
			pm.has_is_null = true;
			pm.is_null = true;
			pm.which_value = 0;
		}

		if (!pb_encode_tag_for_field(stream, field) ||
		    !pb_encode_submessage(stream, PB_METRIC_FIELDS, &pm)) {
			return false;
		}
	}
	return true;
}

int spb_encode(const struct spb_payload_out *p, uint8_t *buf, size_t size, size_t *len)
{
	pb_payload_t msg = org_eclipse_tahu_protobuf_Payload_init_zero;
	struct enc_state st = {.p = p, .err = 0};
	pb_ostream_t stream = pb_ostream_from_buffer(buf, size);

	msg.has_timestamp = true;
	msg.timestamp = p->timestamp;
	msg.has_seq = p->has_seq;
	msg.seq = p->seq;
	msg.metrics.funcs.encode = encode_metrics;
	msg.metrics.arg = &st;

	if (!pb_encode(&stream, PB_PAYLOAD_FIELDS, &msg)) {
		return (st.err != 0) ? st.err : -ENOMEM;
	}
	*len = stream.bytes_written;
	return 0;
}

/* ---- decoding ---------------------------------------------------------- */

struct dec_state {
	spb_metric_cb_t cb;
	void *ctx;
};

static void convert_in(const pb_metric_t *pm, struct spb_metric_in *m)
{
	memset(m, 0, sizeof(*m));
	m->has_name = pm->has_name;
	if (pm->has_name) {
		memcpy(m->name, pm->name, sizeof(m->name));
	}
	m->has_alias = pm->has_alias;
	m->alias = pm->alias;
	m->has_datatype = pm->has_datatype;
	m->datatype = pm->datatype;
	m->is_null = pm->has_is_null && pm->is_null;

	switch (pm->which_value) {
	case PB_TAG(int_value):
		m->kind = SPB_VAL_INT;
		m->v.i = pm->value.int_value;
		break;
	case PB_TAG(long_value):
		m->kind = SPB_VAL_LONG;
		m->v.l = pm->value.long_value;
		break;
	case PB_TAG(float_value):
		m->kind = SPB_VAL_FLOAT;
		m->v.f = pm->value.float_value;
		break;
	case PB_TAG(double_value):
		m->kind = SPB_VAL_DOUBLE;
		m->v.d = pm->value.double_value;
		break;
	case PB_TAG(boolean_value):
		m->kind = SPB_VAL_BOOL;
		m->v.b = pm->value.boolean_value;
		break;
	case PB_TAG(string_value):
		m->kind = SPB_VAL_STRING;
		memcpy(m->v.s, pm->value.string_value, sizeof(m->v.s));
		break;
	default:
		m->kind = SPB_VAL_NONE;
		break;
	}
}

static bool decode_metric(pb_istream_t *stream, const pb_field_t *field, void **arg)
{
	struct dec_state *st = *arg;
	pb_metric_t pm = org_eclipse_tahu_protobuf_Payload_Metric_init_zero;
	struct spb_metric_in m;

	ARG_UNUSED(field);

	if (!pb_decode(stream, PB_METRIC_FIELDS, &pm)) {
		return false;
	}
	convert_in(&pm, &m);
	st->cb(st->ctx, &m);
	return true;
}

int spb_decode(const uint8_t *buf, size_t len, spb_metric_cb_t cb, void *ctx)
{
	pb_payload_t msg = org_eclipse_tahu_protobuf_Payload_init_zero;
	struct dec_state st = {.cb = cb, .ctx = ctx};
	pb_istream_t stream = pb_istream_from_buffer(buf, len);

	msg.metrics.funcs.decode = decode_metric;
	msg.metrics.arg = &st;

	return pb_decode(&stream, PB_PAYLOAD_FIELDS, &msg) ? 0 : -EINVAL;
}

static bool dt_is_signed(uint32_t dt)
{
	return dt == SPB_DT_INT8 || dt == SPB_DT_INT16 || dt == SPB_DT_INT32 ||
	       dt == SPB_DT_INT64;
}

int spb_metric_in_number(const struct spb_metric_in *m, uint32_t datatype, double *out)
{
	if (m->is_null) {
		return -EINVAL;
	}
	switch (m->kind) {
	case SPB_VAL_INT:
		*out = dt_is_signed(datatype) ? (double)(int32_t)m->v.i : (double)m->v.i;
		return 0;
	case SPB_VAL_LONG:
		*out = dt_is_signed(datatype) ? (double)(int64_t)m->v.l : (double)m->v.l;
		return 0;
	case SPB_VAL_FLOAT:
		*out = m->v.f;
		return 0;
	case SPB_VAL_DOUBLE:
		*out = m->v.d;
		return 0;
	case SPB_VAL_BOOL:
		*out = m->v.b ? 1.0 : 0.0;
		return 0;
	default:
		return -EINVAL;
	}
}

/* ---- topics ------------------------------------------------------------ */

int spb_topic(char *buf, size_t size, const char *group, const char *type, const char *node,
	      const char *device)
{
	int n;

	if (device != NULL) {
		n = snprintf(buf, size, SPB_NAMESPACE "/%s/%s/%s/%s", group, type, node, device);
	} else {
		n = snprintf(buf, size, SPB_NAMESPACE "/%s/%s/%s", group, type, node);
	}
	return (n < 0 || (size_t)n >= size) ? -ENOMEM : n;
}

/* Match one '/'-terminated topic level; advances *pos past the separator. */
static bool match_level(const char *topic, size_t len, size_t *pos, const char *level)
{
	size_t n = strlen(level);

	if (len - *pos < n || memcmp(&topic[*pos], level, n) != 0) {
		return false;
	}
	*pos += n;
	if (*pos == len) {
		return true;
	}
	if (topic[*pos] != '/') {
		return false;
	}
	(*pos)++;
	return true;
}

int spb_topic_parse_cmd(const char *topic, size_t len, const char *group, const char *node,
			char *device, size_t device_size)
{
	size_t pos = 0;
	bool is_node;
	size_t dlen;

	if (!match_level(topic, len, &pos, SPB_NAMESPACE) || pos == len ||
	    !match_level(topic, len, &pos, group) || pos == len) {
		return -EINVAL;
	}
	if (match_level(topic, len, &pos, "NCMD")) {
		is_node = true;
	} else if (match_level(topic, len, &pos, "DCMD")) {
		is_node = false;
	} else {
		return -EINVAL;
	}
	if (pos == len || !match_level(topic, len, &pos, node)) {
		return -EINVAL;
	}
	if (is_node) {
		/* match_level() stops at the end or right after a '/' */
		return (pos == len && topic[len - 1] != '/') ? SPB_CMD_NODE : -EINVAL;
	}
	if (pos == len || topic[len - 1] == '/') {
		return -EINVAL;
	}
	dlen = len - pos;
	if (memchr(&topic[pos], '/', dlen) != NULL || dlen >= device_size) {
		return -EINVAL;
	}
	memcpy(device, &topic[pos], dlen);
	device[dlen] = '\0';
	return SPB_CMD_DEVICE;
}
