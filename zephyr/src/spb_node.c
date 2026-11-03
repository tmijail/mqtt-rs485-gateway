/*
 * Sparkplug B 3.0 edge node: BIRTH/DATA/DEATH generation and command
 * handling. Runs exclusively in the MQTT session thread.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "spb_node.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "convert.h"
#include "model.h"
#include "sparkplug.h"

LOG_MODULE_REGISTER(spb_node, CONFIG_GW_LOG_LEVEL);

#define TOPIC_MAX 128

/* Node metric table (SPB-4): order defines NBIRTH order. */
enum node_item {
	NI_BDSEQ,
	NI_REBIRTH,
	NI_REBOOT,
	NI_FIRMWARE,
	NI_HARDWARE,
	NI_DI_FIRST,
	NI_DO_FIRST = NI_DI_FIRST + GW_DI_COUNT,
	NI_COUNT = NI_DO_FIRST + GW_DO_COUNT,
};

static const char *const node_names[NI_COUNT] = {
	[NI_BDSEQ] = "bdSeq",
	[NI_REBIRTH] = "Node Control/Rebirth",
	[NI_REBOOT] = "Node Control/Reboot",
	[NI_FIRMWARE] = "Node Info/Firmware",
	[NI_HARDWARE] = "Node Info/Hardware",
	[NI_DI_FIRST + 0] = "DI/1", [NI_DI_FIRST + 1] = "DI/2", [NI_DI_FIRST + 2] = "DI/3",
	[NI_DI_FIRST + 3] = "DI/4", [NI_DI_FIRST + 4] = "DI/5", [NI_DI_FIRST + 5] = "DI/6",
	[NI_DI_FIRST + 6] = "DI/7", [NI_DI_FIRST + 7] = "DI/8",
	[NI_DO_FIRST + 0] = "DO/1", [NI_DO_FIRST + 1] = "DO/2", [NI_DO_FIRST + 2] = "DO/3",
	[NI_DO_FIRST + 3] = "DO/4", [NI_DO_FIRST + 4] = "DO/5", [NI_DO_FIRST + 5] = "DO/6",
	[NI_DO_FIRST + 6] = "DO/7", [NI_DO_FIRST + 7] = "DO/8",
};

static uint64_t node_alias(enum node_item item)
{
	switch (item) {
	case NI_BDSEQ:
		return SPB_ALIAS_BDSEQ;
	case NI_REBIRTH:
		return SPB_ALIAS_REBIRTH;
	case NI_REBOOT:
		return SPB_ALIAS_REBOOT;
	case NI_FIRMWARE:
		return SPB_ALIAS_FIRMWARE;
	case NI_HARDWARE:
		return SPB_ALIAS_HARDWARE;
	default:
		return (item < NI_DO_FIRST) ? SPB_ALIAS_DI_BASE + (item - NI_DI_FIRST)
					    : SPB_ALIAS_DO_BASE + (item - NI_DO_FIRST);
	}
}

static int node_item_by_alias(uint64_t alias)
{
	for (int i = 0; i < NI_COUNT; i++) {
		if (node_alias((enum node_item)i) == alias) {
			return i;
		}
	}
	return -ENOENT;
}

static int node_item_by_name(const char *name)
{
	for (int i = 0; i < NI_COUNT; i++) {
		if (strcmp(node_names[i], name) == 0) {
			return i;
		}
	}
	return -ENOENT;
}

/* Snapshot entry used to encode device payloads outside the model lock. */
struct tag_snap {
	uint16_t tag;
	bool valid;
	double value;
	int64_t ts_ms;
};

struct node_snap {
	bool birth;
	uint8_t count;
	uint8_t items[NI_COUNT];
	uint8_t di;
	uint8_t outputs;
	int64_t di_ts_ms;
	int64_t do_ts_ms;
	uint64_t now_ms;
};

static struct {
	const struct gw_config *cfg;
	const struct spb_node_ops *ops;
	const char *firmware;
	const char *hardware;
	bool active;
	bool rebirth;
	bool reboot;
	uint8_t bdseq;
	uint8_t seq;
	uint8_t reported_di;
	uint8_t reported_do;
	bool dev_born[CONFIG_GW_MAX_DEVICES];
	uint32_t dev_born_gen[CONFIG_GW_MAX_DEVICES];
	struct tag_snap snap[CONFIG_GW_MAX_TAGS_PER_DEVICE];
	uint8_t payload[CONFIG_GW_PAYLOAD_BUF_SIZE];
	char topic[TOPIC_MAX];
} n;

void spb_node_init(const struct gw_config *cfg, const struct spb_node_ops *ops,
		   const char *firmware, const char *hardware)
{
	memset(&n, 0, sizeof(n));
	n.cfg = cfg;
	n.ops = ops;
	n.firmware = firmware;
	n.hardware = hardware;
}

uint8_t spb_node_seq(void)
{
	return n.seq;
}

static uint8_t next_seq(void)
{
	return n.seq++; /* wraps 255 -> 0 (SPB-5) */
}

static int publish(const char *type, const char *device, const struct spb_payload_out *p,
		   uint8_t qos)
{
	size_t len;
	int ret;

	ret = spb_encode(p, n.payload, sizeof(n.payload), &len);
	if (ret < 0) {
		LOG_ERR("%s%s%s: encode failed (%d)", type, device ? "/" : "", device ? device : "",
			ret);
		return ret;
	}
	ret = spb_topic(n.topic, sizeof(n.topic), n.cfg->sparkplug.group_id, type,
			n.cfg->sparkplug.edge_node_id, device);
	if (ret < 0) {
		return ret;
	}
	ret = n.ops->publish(n.topic, n.payload, len, qos);
	if (ret < 0) {
		LOG_WRN("publish %s failed (%d)", n.topic, ret);
	}
	return ret;
}

/* ---- tag metric helpers ------------------------------------------------ */

static void tag_value(const struct gw_tag_cfg *t, double value, struct spb_metric *m)
{
	m->datatype = gw_tag_datatype(t);
	switch (m->datatype) {
	case SPB_DT_BOOLEAN:
		m->v.b = value != 0.0;
		break;
	case SPB_DT_INT16:
	case SPB_DT_INT32:
		m->v.i = (int64_t)value;
		break;
	case SPB_DT_UINT16:
	case SPB_DT_UINT32:
		m->v.u = (uint64_t)value;
		break;
	default:
		m->v.d = value;
		break;
	}
}

static int get_dev_birth_metric(void *ctx, size_t i, struct spb_metric *m)
{
	const struct tag_snap *s = &n.snap[i];
	const struct gw_tag_cfg *t = &n.cfg->tags[s->tag];

	ARG_UNUSED(ctx);
	m->name = t->name;
	m->has_alias = true;
	m->alias = SPB_ALIAS_TAG_BASE + s->tag;
	m->has_datatype = true;
	tag_value(t, s->value, m);
	m->is_null = !s->valid;
	m->timestamp = s->valid ? (uint64_t)s->ts_ms : 0;
	return 0;
}

static int get_dev_data_metric(void *ctx, size_t i, struct spb_metric *m)
{
	const struct tag_snap *s = &n.snap[i];

	ARG_UNUSED(ctx);
	m->has_alias = true;
	m->alias = SPB_ALIAS_TAG_BASE + s->tag;
	tag_value(&n.cfg->tags[s->tag], s->value, m);
	m->timestamp = (uint64_t)s->ts_ms;
	return 0;
}

/* ---- node metric helpers ----------------------------------------------- */

static int get_node_metric(void *ctx, size_t i, struct spb_metric *m)
{
	const struct node_snap *ns = ctx;
	enum node_item item = (enum node_item)ns->items[i];
	bool birth = ns->birth;

	m->name = birth ? node_names[item] : NULL;
	m->has_alias = true;
	m->alias = node_alias(item);
	m->has_datatype = birth;
	m->timestamp = ns->now_ms;

	switch (item) {
	case NI_BDSEQ:
		m->datatype = SPB_DT_UINT64;
		m->v.u = n.bdseq;
		break;
	case NI_REBIRTH:
	case NI_REBOOT:
		m->datatype = SPB_DT_BOOLEAN;
		m->v.b = false;
		break;
	case NI_FIRMWARE:
		m->datatype = SPB_DT_STRING;
		m->v.s = n.firmware;
		break;
	case NI_HARDWARE:
		m->datatype = SPB_DT_STRING;
		m->v.s = n.hardware;
		break;
	default:
		m->datatype = SPB_DT_BOOLEAN;
		if (item < NI_DO_FIRST) {
			m->v.b = (ns->di & BIT(item - NI_DI_FIRST)) != 0;
			m->timestamp = (uint64_t)ns->di_ts_ms;
		} else {
			m->v.b = (ns->outputs & BIT(item - NI_DO_FIRST)) != 0;
			m->timestamp = (uint64_t)ns->do_ts_ms;
		}
		break;
	}
	return 0;
}

static void snap_io(struct node_snap *ns)
{
	const struct gw_io_state *io;

	model_lock();
	io = model_io();
	ns->di = io->di;
	ns->outputs = io->outputs;
	ns->di_ts_ms = io->di_ts_ms;
	ns->do_ts_ms = io->do_ts_ms;
	model_unlock();
}

static int publish_nbirth(void)
{
	struct node_snap ns = {.birth = true, .count = NI_COUNT};
	struct spb_payload_out p;

	for (int i = 0; i < NI_COUNT; i++) {
		ns.items[i] = (uint8_t)i;
	}
	(void)model_take_node_dirty();
	snap_io(&ns);
	ns.now_ms = (uint64_t)n.ops->now_ms();
	/* Births report the current state: nothing is pending afterwards. */
	n.reported_di = ns.di;
	n.reported_do = ns.outputs;

	n.seq = 0; /* SPB-4, SPB-9 */
	p = (struct spb_payload_out){
		.timestamp = ns.now_ms,
		.has_seq = true,
		.seq = next_seq(),
		.metric_count = NI_COUNT,
		.get = get_node_metric,
		.ctx = &ns,
	};
	LOG_INF("NBIRTH bdSeq=%u", n.bdseq);
	return publish("NBIRTH", NULL, &p, 0);
}

static int publish_ndata(void)
{
	struct node_snap ns = {0};
	struct spb_payload_out p;
	uint8_t di_changed;
	uint8_t do_changed;

	snap_io(&ns);
	di_changed = ns.di ^ n.reported_di;
	do_changed = ns.outputs ^ n.reported_do;
	for (int i = 0; i < GW_DI_COUNT; i++) {
		if (di_changed & BIT(i)) {
			ns.items[ns.count++] = (uint8_t)(NI_DI_FIRST + i);
		}
	}
	for (int i = 0; i < GW_DO_COUNT; i++) {
		if (do_changed & BIT(i)) {
			ns.items[ns.count++] = (uint8_t)(NI_DO_FIRST + i);
		}
	}
	if (ns.count == 0) {
		return 0;
	}
	n.reported_di = ns.di;
	n.reported_do = ns.outputs;
	ns.now_ms = (uint64_t)n.ops->now_ms();
	p = (struct spb_payload_out){
		.timestamp = ns.now_ms,
		.has_seq = true,
		.seq = next_seq(),
		.metric_count = ns.count,
		.get = get_node_metric,
		.ctx = &ns,
	};
	return publish("NDATA", NULL, &p, 0);
}

/* ---- device messages --------------------------------------------------- */

static int publish_dbirth(uint8_t dev)
{
	const struct gw_device_cfg *d = &n.cfg->devices[dev];
	struct spb_payload_out p;

	model_lock();
	for (uint16_t i = 0; i < d->tag_count; i++) {
		uint16_t tag = d->first_tag + i;
		struct gw_tag_state *st = model_tag(tag);

		n.snap[i] = (struct tag_snap){
			.tag = tag, .valid = st->valid, .value = st->value, .ts_ms = st->ts_ms};
		st->reported = st->value;
		st->pending = false;
	}
	n.dev_born_gen[dev] = model_dev(dev)->generation;
	model_unlock();

	n.dev_born[dev] = true;
	p = (struct spb_payload_out){
		.timestamp = (uint64_t)n.ops->now_ms(),
		.has_seq = true,
		.seq = next_seq(),
		.metric_count = d->tag_count,
		.get = get_dev_birth_metric,
	};
	LOG_INF("DBIRTH %s", d->name);
	return publish("DBIRTH", d->name, &p, 0);
}

static int publish_ddeath(uint8_t dev)
{
	struct spb_payload_out p = {
		.timestamp = (uint64_t)n.ops->now_ms(),
		.has_seq = true,
		.seq = next_seq(),
	};

	n.dev_born[dev] = false;
	LOG_INF("DDEATH %s", n.cfg->devices[dev].name);
	return publish("DDEATH", n.cfg->devices[dev].name, &p, 0);
}

static int publish_ddata(uint8_t dev)
{
	const struct gw_device_cfg *d = &n.cfg->devices[dev];
	struct spb_payload_out p;
	size_t count = 0;

	model_lock();
	for (uint16_t i = 0; i < d->tag_count; i++) {
		uint16_t tag = d->first_tag + i;
		struct gw_tag_state *st = model_tag(tag);

		if (!st->pending || !st->valid) {
			continue;
		}
		n.snap[count++] = (struct tag_snap){
			.tag = tag, .valid = true, .value = st->value, .ts_ms = st->ts_ms};
		st->reported = st->value;
		st->pending = false;
	}
	model_unlock();

	if (count == 0) {
		return 0;
	}
	p = (struct spb_payload_out){
		.timestamp = (uint64_t)n.ops->now_ms(),
		.has_seq = true,
		.seq = next_seq(),
		.metric_count = count,
		.get = get_dev_data_metric,
	};
	return publish("DDATA", d->name, &p, 0);
}

/* Reconcile what the host has been told about @p dev with its real state. */
static int service_device(uint8_t dev, bool dirty)
{
	bool online;
	uint32_t gen;

	model_lock();
	online = model_dev(dev)->online;
	gen = model_dev(dev)->generation;
	model_unlock();

	if (online && (!n.dev_born[dev] || n.dev_born_gen[dev] != gen)) {
		return publish_dbirth(dev);
	}
	if (!online && n.dev_born[dev]) {
		return publish_ddeath(dev);
	}
	if (online && dirty) {
		return publish_ddata(dev);
	}
	return 0;
}

/* ---- public API -------------------------------------------------------- */

static int get_death_metric(void *ctx, size_t i, struct spb_metric *m)
{
	ARG_UNUSED(i);
	m->name = node_names[NI_BDSEQ];
	m->has_datatype = true;
	m->datatype = SPB_DT_UINT64;
	m->v.u = *(const uint8_t *)ctx;
	return 0;
}

int spb_node_encode_death(uint8_t bdseq, uint8_t *buf, size_t size, size_t *len)
{
	struct spb_payload_out p = {
		.timestamp = (uint64_t)n.ops->now_ms(),
		.has_seq = false, /* NDEATH carries no seq (SPB-5) */
		.metric_count = 1,
		.get = get_death_metric,
		.ctx = &bdseq,
	};

	return spb_encode(&p, buf, size, len);
}

void spb_node_session_start(uint8_t bdseq)
{
	n.bdseq = bdseq;
	n.active = true;
	n.rebirth = true;
	n.reboot = false;
	memset(n.dev_born, 0, sizeof(n.dev_born));
}

void spb_node_session_end(void)
{
	n.active = false;
	memset(n.dev_born, 0, sizeof(n.dev_born));
}

int spb_node_service(void)
{
	uint32_t dirty;
	int ret = 0;

	if (!n.active) {
		return 0;
	}
	if (n.reboot) {
		n.reboot = false;
		return SPB_NODE_REQ_REBOOT;
	}
	if (n.rebirth) {
		n.rebirth = false;
		memset(n.dev_born, 0, sizeof(n.dev_born));
		ret = publish_nbirth();
		if (ret < 0) {
			return ret;
		}
	} else if (model_take_node_dirty()) {
		ret = publish_ndata();
		if (ret < 0) {
			return ret;
		}
	}

	dirty = model_take_dirty_devices();
	for (uint8_t dev = 0; dev < n.cfg->device_count; dev++) {
		ret = service_device(dev, (dirty & BIT(dev)) != 0);
		if (ret < 0) {
			/* The session is dropped; the next one rebirths everything. */
			return ret;
		}
	}
	return SPB_NODE_REQ_NONE;
}

/* ---- commands (SPB-9, SPB-10, SPB-11) ---------------------------------- */

struct cmd_ctx {
	uint8_t dev;
};

static bool metric_bool(const struct spb_metric_in *m, bool *out)
{
	double v;

	if (spb_metric_in_number(m, SPB_DT_BOOLEAN, &v) < 0) {
		return false;
	}
	*out = v != 0.0;
	return true;
}

static const char *metric_label(const struct spb_metric_in *m, char *buf, size_t size)
{
	if (m->has_name) {
		return m->name;
	}
	snprintf(buf, size, "alias %llu", (unsigned long long)m->alias);
	return buf;
}

static void on_ncmd_metric(void *ctx, const struct spb_metric_in *m)
{
	char label[32];
	int item = -ENOENT;
	bool on;

	ARG_UNUSED(ctx);
	if (m->has_alias) {
		item = node_item_by_alias(m->alias);
	} else if (m->has_name) {
		item = node_item_by_name(m->name);
	}
	if (item < 0) {
		LOG_WRN("NCMD %s: unknown metric, discarded", metric_label(m, label, sizeof(label)));
		return;
	}
	if (!metric_bool(m, &on)) {
		LOG_WRN("NCMD %s: missing or non-boolean value, discarded", node_names[item]);
		return;
	}
	if (item == NI_REBIRTH) {
		if (on) {
			LOG_INF("NCMD rebirth requested");
			n.rebirth = true;
		}
	} else if (item == NI_REBOOT) {
		if (on) {
			LOG_WRN("NCMD reboot requested");
			n.reboot = true;
		}
	} else if (item >= NI_DO_FIRST) {
		int ret = n.ops->set_output((uint8_t)(item - NI_DO_FIRST), on);

		if (ret < 0) {
			LOG_ERR("NCMD %s=%d failed (%d)", node_names[item], on, ret);
		}
	} else {
		LOG_WRN("NCMD %s: read-only metric, discarded", node_names[item]);
	}
}

static int find_tag(uint8_t dev, const struct spb_metric_in *m)
{
	const struct gw_device_cfg *d = &n.cfg->devices[dev];

	if (m->has_alias) {
		if (m->alias >= SPB_ALIAS_TAG_BASE + d->first_tag &&
		    m->alias < SPB_ALIAS_TAG_BASE + d->first_tag + d->tag_count) {
			return (int)(m->alias - SPB_ALIAS_TAG_BASE);
		}
		return -ENOENT;
	}
	if (m->has_name) {
		for (uint16_t i = 0; i < d->tag_count; i++) {
			if (strcmp(n.cfg->tags[d->first_tag + i].name, m->name) == 0) {
				return d->first_tag + i;
			}
		}
	}
	return -ENOENT;
}

static void on_dcmd_metric(void *ctx, const struct spb_metric_in *m)
{
	const struct cmd_ctx *c = ctx;
	const char *dev_name = n.cfg->devices[c->dev].name;
	const struct gw_tag_cfg *t;
	char label[32];
	double value;
	int tag;
	int ret;

	tag = find_tag(c->dev, m);
	if (tag < 0) {
		LOG_WRN("DCMD %s/%s: unknown metric, discarded", dev_name,
			metric_label(m, label, sizeof(label)));
		return;
	}
	t = &n.cfg->tags[tag];
	if (!t->writable) {
		LOG_WRN("DCMD %s/%s: read-only metric, discarded", dev_name, t->name);
		return;
	}
	if (spb_metric_in_number(m, m->has_datatype ? m->datatype : gw_tag_datatype(t), &value) <
	    0) {
		LOG_WRN("DCMD %s/%s: missing or non-numeric value, discarded", dev_name, t->name);
		return;
	}
	ret = n.ops->write_tag(c->dev, (uint16_t)tag, value);
	if (ret < 0) {
		LOG_WRN("DCMD %s/%s: write not queued (%d)", dev_name, t->name, ret);
	}
}

void spb_node_on_command(const char *topic, size_t topic_len, const uint8_t *payload,
			 size_t len)
{
	char device[GW_NAME_MAX + 1];
	struct cmd_ctx ctx = {0};
	spb_metric_cb_t cb;
	int target;

	target = spb_topic_parse_cmd(topic, topic_len, n.cfg->sparkplug.group_id,
				     n.cfg->sparkplug.edge_node_id, device, sizeof(device));
	if (target == SPB_CMD_NODE) {
		cb = on_ncmd_metric;
	} else if (target == SPB_CMD_DEVICE) {
		bool found = false;

		for (uint8_t i = 0; i < n.cfg->device_count; i++) {
			if (strcmp(n.cfg->devices[i].name, device) == 0) {
				ctx.dev = i;
				found = true;
				break;
			}
		}
		if (!found) {
			LOG_WRN("DCMD for unknown device '%s' discarded", device);
			return;
		}
		cb = on_dcmd_metric;
	} else {
		LOG_WRN("message on unexpected topic %.*s discarded", (int)topic_len, topic);
		return;
	}
	if (spb_decode(payload, len, cb, &ctx) < 0) {
		LOG_WRN("malformed %s payload (%u bytes) discarded",
			target == SPB_CMD_NODE ? "NCMD" : "DCMD", (unsigned int)len);
	}
}
