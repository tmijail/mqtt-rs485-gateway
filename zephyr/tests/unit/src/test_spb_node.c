/*
 * Sparkplug edge node behaviour (SPB-2..SPB-11) with fake transport,
 * Modbus and I/O. Each published payload is also printed as
 * "SPB_SAMPLE <topic> <hex>" so scripts/decode_samples.py can check it
 * against the official proto with Python protobuf.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "convert.h"
#include "model.h"
#include "sparkplug.h"
#include "spb_node.h"
#include "test_util.h"

#define MAX_MSGS 16

struct msg {
	char topic[128];
	uint8_t data[1024];
	size_t len;
	uint8_t qos;
};

static struct msg msgs[MAX_MSGS];
static size_t n_msgs;
static int64_t now;
static int writes;
static struct {
	uint8_t dev;
	uint16_t tag;
	double value;
} last_write;
static int outputs_set;
static bool quiet; /* suppress SPB_SAMPLE output in bulk tests */

static void dump(const struct msg *m)
{
	char hex[2 * sizeof(m->data) + 1];

	for (size_t i = 0; i < m->len; i++) {
		snprintf(&hex[2 * i], 3, "%02x", m->data[i]);
	}
	hex[2 * m->len] = '\0';
	printk("SPB_SAMPLE %s %s\n", m->topic, hex);
}

static int fake_publish(const char *topic, const uint8_t *payload, size_t len, uint8_t qos)
{
	struct msg *m = &msgs[n_msgs % MAX_MSGS];

	zassert_true(len <= sizeof(m->data));
	strncpy(m->topic, topic, sizeof(m->topic) - 1);
	memcpy(m->data, payload, len);
	m->len = len;
	m->qos = qos;
	n_msgs++;
	if (!quiet) {
		dump(m);
	}
	return 0;
}

static int fake_write(uint8_t dev, uint16_t tag, double value)
{
	writes++;
	last_write.dev = dev;
	last_write.tag = tag;
	last_write.value = value;
	return 0;
}

static int fake_set_output(uint8_t channel, bool on)
{
	outputs_set++;
	model_lock();
	WRITE_BIT(model_io()->outputs, channel, on);
	model_io()->do_ts_ms = now;
	model_unlock();
	model_signal_node();
	return 0;
}

static int64_t fake_now(void)
{
	return now;
}

static const struct spb_node_ops ops = {
	.publish = fake_publish,
	.write_tag = fake_write,
	.set_output = fake_set_output,
	.now_ms = fake_now,
};

/* ---- helpers ------------------------------------------------------------ */

struct decoded {
	size_t n;
	struct spb_metric_in m[24];
};

static void collect(void *ctx, const struct spb_metric_in *m)
{
	struct decoded *d = ctx;

	if (d->n < ARRAY_SIZE(d->m)) {
		d->m[d->n] = *m;
	}
	d->n++;
}

static const struct msg *last(void)
{
	zassert_true(n_msgs > 0);
	return &msgs[(n_msgs - 1) % MAX_MSGS];
}

/* Decode a message and return its seq (-1 when absent). */
static int decode(const struct msg *m, struct decoded *d)
{
	int seq = -1;
	const uint8_t *p = m->data;

	memset(d, 0, sizeof(*d));
	zassert_ok(spb_decode(m->data, m->len, collect, d));
	/* seq is field 3 (varint): scan the top level for it. */
	for (size_t i = 0; i < m->len;) {
		uint8_t key = p[i++];
		uint64_t v = 0;
		int shift = 0;

		if ((key & 7) == 0) {
			do {
				v |= (uint64_t)(p[i] & 0x7f) << shift;
				shift += 7;
			} while (p[i++] & 0x80);
			if ((key >> 3) == 3) {
				seq = (int)v;
			}
		} else if ((key & 7) == 2) {
			uint64_t l = 0;

			shift = 0;
			do {
				l |= (uint64_t)(p[i] & 0x7f) << shift;
				shift += 7;
			} while (p[i++] & 0x80);
			i += l;
		} else {
			break;
		}
	}
	return seq;
}

static const struct spb_metric_in *find(const struct decoded *d, const char *name,
					uint64_t alias)
{
	for (size_t i = 0; i < d->n; i++) {
		if ((name && d->m[i].has_name && strcmp(d->m[i].name, name) == 0) ||
		    (!name && d->m[i].has_alias && d->m[i].alias == alias)) {
			return &d->m[i];
		}
	}
	return NULL;
}

/* Encode a single-metric command and hand it to the node. */
static struct spb_metric cmd_metric;

static int get_cmd_metric(void *ctx, size_t i, struct spb_metric *out)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(i);
	*out = cmd_metric;
	return 0;
}

static void send_cmd(const char *topic, struct spb_metric m)
{
	struct spb_payload_out p = {.timestamp = 1, .metric_count = 1, .get = get_cmd_metric};
	uint8_t buf[128];
	size_t len;

	cmd_metric = m;
	zassert_ok(spb_encode(&p, buf, sizeof(buf), &len));
	spb_node_on_command(topic, strlen(topic), buf, len);
}

static void device_online(uint8_t dev, double voltage)
{
	model_lock();
	model_tag_store(0, voltage, now, false);
	model_tag_store(1, 12.0, now, false);
	model_tag_store(2, 0.0, now, false);
	model_dev(dev)->online = true;
	model_dev(dev)->generation++;
	model_unlock();
	model_signal_device(dev);
}

static void store(uint16_t tag, double value)
{
	model_lock();
	model_tag_store(tag, value, ++now, false);
	model_unlock();
	model_signal_device(0);
}

/* ---- fixture ------------------------------------------------------------ */

static void before(void *f)
{
	const struct gw_config *cfg = test_config_load(spec_config_json);

	ARG_UNUSED(f);
	memset(msgs, 0, sizeof(msgs));
	n_msgs = 0;
	writes = 0;
	outputs_set = 0;
	quiet = false;
	now = 1700000000000LL;
	model_init(cfg);
	spb_node_init(cfg, &ops, "0.1.0+test", "Test HW");
}

/* ---- tests -------------------------------------------------------------- */

ZTEST(spb_node, test_birth_sequence)
{
	struct decoded d;
	const struct spb_metric_in *m;

	spb_node_session_start(7);
	zassert_equal(spb_node_service(), SPB_NODE_REQ_NONE);
	zassert_equal(n_msgs, 1, "offline device: only NBIRTH");
	zassert_str_equal(last()->topic, "spBv1.0/Plant1/NBIRTH/GW01");
	zassert_equal(last()->qos, 0);
	zassert_equal(decode(last(), &d), 0, "NBIRTH seq is 0");
	zassert_equal(d.n, 21);
	m = find(&d, "bdSeq", 0);
	zassert_not_null(m);
	zassert_equal(m->datatype, SPB_DT_UINT64);
	zassert_equal(m->v.l, 7, "NBIRTH carries the will's bdSeq");
	m = find(&d, "Node Control/Rebirth", 0);
	zassert_true(m && m->kind == SPB_VAL_BOOL && !m->v.b);
	zassert_not_null(find(&d, "Node Control/Reboot", 0));
	zassert_str_equal(find(&d, "Node Info/Firmware", 0)->v.s, "0.1.0+test");
	zassert_str_equal(find(&d, "Node Info/Hardware", 0)->v.s, "Test HW");
	zassert_equal(find(&d, "DI/1", 0)->alias, SPB_ALIAS_DI_BASE);
	zassert_equal(find(&d, "DO/8", 0)->alias, SPB_ALIAS_DO_BASE + 7);
	for (size_t i = 0; i < d.n; i++) {
		zassert_true(d.m[i].has_name && d.m[i].has_alias && d.m[i].has_datatype &&
			     d.m[i].kind != SPB_VAL_NONE, "metric %u incomplete", i);
	}

	device_online(0, 230.0);
	zassert_equal(spb_node_service(), SPB_NODE_REQ_NONE);
	zassert_str_equal(last()->topic, "spBv1.0/Plant1/DBIRTH/GW01/Meter1");
	zassert_equal(decode(last(), &d), 1);
	zassert_equal(d.n, 3);
	m = find(&d, "Voltage", 0);
	zassert_true(m && m->alias == SPB_ALIAS_TAG_BASE && m->datatype == SPB_DT_FLOAT);
	zassert_within((double)m->v.f, 230.0, 1e-3);
	m = find(&d, "Setpoint", 0);
	zassert_true(m && m->datatype == SPB_DT_FLOAT && m->alias == SPB_ALIAS_TAG_BASE + 1);
	m = find(&d, "Run", 0);
	zassert_true(m && m->datatype == SPB_DT_BOOLEAN && !m->v.b);

	zassert_equal(spb_node_service(), SPB_NODE_REQ_NONE);
	zassert_equal(n_msgs, 2, "nothing changed: nothing published");
}

ZTEST(spb_node, test_report_by_exception)
{
	struct decoded d;

	spb_node_session_start(0);
	device_online(0, 230.0);
	(void)spb_node_service();
	zassert_equal(n_msgs, 2);

	store(0, 230.05);
	store(2, 0.0);
	(void)spb_node_service();
	zassert_equal(n_msgs, 2, "inside the deadband / unchanged: no DDATA");

	store(0, 230.3);
	store(2, 1.0);
	(void)spb_node_service();
	zassert_equal(n_msgs, 3);
	zassert_str_equal(last()->topic, "spBv1.0/Plant1/DDATA/GW01/Meter1");
	zassert_equal(decode(last(), &d), 2);
	zassert_equal(d.n, 2, "one message for the cycle, changed metrics only");
	for (size_t i = 0; i < d.n; i++) {
		zassert_false(d.m[i].has_name, "DATA metrics carry no name");
	}
	zassert_within((double)find(&d, NULL, SPB_ALIAS_TAG_BASE)->v.f, 230.3, 1e-3);
	zassert_true(find(&d, NULL, SPB_ALIAS_TAG_BASE + 2)->v.b);
}

ZTEST(spb_node, test_device_death_and_rebirth)
{
	struct decoded d;

	spb_node_session_start(0);
	device_online(0, 230.0);
	(void)spb_node_service();

	model_lock();
	model_dev(0)->online = false;
	model_unlock();
	model_signal_device(0);
	(void)spb_node_service();
	zassert_str_equal(last()->topic, "spBv1.0/Plant1/DDEATH/GW01/Meter1");
	zassert_equal(decode(last(), &d), 2, "DDEATH takes the next seq");

	device_online(0, 231.0);
	(void)spb_node_service();
	zassert_str_equal(last()->topic, "spBv1.0/Plant1/DBIRTH/GW01/Meter1");
	zassert_equal(decode(last(), &d), 3);
}

ZTEST(spb_node, test_ncmd_rebirth)
{
	struct decoded d;

	spb_node_session_start(5);
	device_online(0, 230.0);
	(void)spb_node_service();
	store(0, 240.0);
	(void)spb_node_service();
	zassert_equal(n_msgs, 3);

	send_cmd("spBv1.0/Plant1/NCMD/GW01", (struct spb_metric){
		.name = "Node Control/Rebirth", .has_datatype = true,
		.datatype = SPB_DT_BOOLEAN, .v.b = true});
	(void)spb_node_service();
	zassert_equal(n_msgs, 5, "NBIRTH and DBIRTH again");
	zassert_str_equal(msgs[3].topic, "spBv1.0/Plant1/NBIRTH/GW01");
	zassert_equal(decode(&msgs[3], &d), 0, "seq restarts at 0");
	zassert_equal(find(&d, "bdSeq", 0)->v.l, 5, "same bdSeq on rebirth");
	zassert_str_equal(msgs[4].topic, "spBv1.0/Plant1/DBIRTH/GW01/Meter1");
	zassert_equal(decode(&msgs[4], &d), 1);
	zassert_within((double)find(&d, "Voltage", 0)->v.f, 240.0, 1e-3, "current values");
}

ZTEST(spb_node, test_seq_wraps)
{
	struct decoded d;
	int prev = 0;

	quiet = true;
	spb_node_session_start(0);
	(void)spb_node_service();
	for (int i = 0; i < 300; i++) {
		int seq;

		(void)fake_set_output(0, (i % 2) == 0);
		(void)spb_node_service();
		seq = decode(last(), &d);
		zassert_str_equal(last()->topic, "spBv1.0/Plant1/NDATA/GW01");
		zassert_equal(seq, (prev + 1) % 256, "i=%d", i);
		prev = seq;
	}
}

ZTEST(spb_node, test_dcmd)
{
	uint16_t regs[2];

	spb_node_session_start(0);
	device_online(0, 230.0);
	(void)spb_node_service();

	/* SPEC 7.4: Setpoint=12.3 by name, as a Float */
	send_cmd("spBv1.0/Plant1/DCMD/GW01/Meter1", (struct spb_metric){
		.name = "Setpoint", .has_datatype = true, .datatype = SPB_DT_FLOAT,
		.v.d = 12.3});
	zassert_equal(writes, 1);
	zassert_equal(last_write.dev, 0);
	zassert_equal(last_write.tag, 1);
	zassert_ok(gw_value_to_regs(&model_cfg()->tags[1], last_write.value, regs));
	zassert_equal(regs[0], 123, "12.3 / 0.1 = 123 on the wire");

	/* By alias, Boolean */
	send_cmd("spBv1.0/Plant1/DCMD/GW01/Meter1", (struct spb_metric){
		.has_alias = true, .alias = SPB_ALIAS_TAG_BASE + 2, .datatype = SPB_DT_BOOLEAN,
		.v.b = true});
	zassert_equal(writes, 2);
	zassert_equal(last_write.tag, 2);
	zassert_equal(last_write.value, 1.0);

	/* Rejected: read-only, unknown name, alias of another device's range,
	 * unknown device, non-numeric value.
	 */
	send_cmd("spBv1.0/Plant1/DCMD/GW01/Meter1", (struct spb_metric){
		.name = "Voltage", .datatype = SPB_DT_FLOAT, .v.d = 1});
	send_cmd("spBv1.0/Plant1/DCMD/GW01/Meter1", (struct spb_metric){
		.name = "Nope", .datatype = SPB_DT_FLOAT, .v.d = 1});
	send_cmd("spBv1.0/Plant1/DCMD/GW01/Meter1", (struct spb_metric){
		.has_alias = true, .alias = SPB_ALIAS_TAG_BASE + 3, .datatype = SPB_DT_FLOAT});
	send_cmd("spBv1.0/Plant1/DCMD/GW01/Meter9", (struct spb_metric){
		.name = "Setpoint", .datatype = SPB_DT_FLOAT, .v.d = 1});
	send_cmd("spBv1.0/Plant1/DCMD/GW01/Meter1", (struct spb_metric){
		.name = "Setpoint", .datatype = SPB_DT_STRING, .v.s = "12"});
	zassert_equal(writes, 2, "invalid commands are discarded");
}

ZTEST(spb_node, test_ncmd_outputs_and_reboot)
{
	struct decoded d;
	const struct spb_metric_in *m;

	spb_node_session_start(0);
	(void)spb_node_service();

	send_cmd("spBv1.0/Plant1/NCMD/GW01", (struct spb_metric){
		.name = "DO/3", .has_datatype = true, .datatype = SPB_DT_BOOLEAN, .v.b = true});
	zassert_equal(outputs_set, 1);
	(void)spb_node_service();
	zassert_str_equal(last()->topic, "spBv1.0/Plant1/NDATA/GW01");
	zassert_equal(decode(last(), &d), 1);
	zassert_equal(d.n, 1);
	m = find(&d, NULL, SPB_ALIAS_DO_BASE + 2);
	zassert_true(m && m->v.b, "NDATA confirms DO/3=true");

	/* By alias, and read-only DI is refused */
	send_cmd("spBv1.0/Plant1/NCMD/GW01", (struct spb_metric){
		.has_alias = true, .alias = SPB_ALIAS_DO_BASE + 2, .datatype = SPB_DT_BOOLEAN,
		.v.b = false});
	send_cmd("spBv1.0/Plant1/NCMD/GW01", (struct spb_metric){
		.name = "DI/1", .datatype = SPB_DT_BOOLEAN, .v.b = true});
	zassert_equal(outputs_set, 2);

	send_cmd("spBv1.0/Plant1/NCMD/GW01", (struct spb_metric){
		.name = "Node Control/Reboot", .datatype = SPB_DT_BOOLEAN, .v.b = true});
	zassert_equal(spb_node_service(), SPB_NODE_REQ_REBOOT);
}

ZTEST(spb_node, test_ndeath)
{
	struct msg m = {.topic = "spBv1.0/Plant1/NDEATH/GW01"};
	struct decoded d;

	zassert_ok(spb_node_encode_death(9, m.data, sizeof(m.data), &m.len));
	dump(&m);
	zassert_equal(decode(&m, &d), -1, "NDEATH has no seq");
	zassert_equal(d.n, 1);
	zassert_str_equal(d.m[0].name, "bdSeq");
	zassert_equal(d.m[0].datatype, SPB_DT_UINT64);
	zassert_equal(d.m[0].v.l, 9);
}

ZTEST(spb_node, test_inactive_session_publishes_nothing)
{
	device_online(0, 230.0);
	zassert_equal(spb_node_service(), SPB_NODE_REQ_NONE);
	zassert_equal(n_msgs, 0);
	spb_node_session_start(0);
	spb_node_session_end();
	zassert_equal(spb_node_service(), SPB_NODE_REQ_NONE);
	zassert_equal(n_msgs, 0);
}

ZTEST_SUITE(spb_node, NULL, NULL, before, NULL, NULL);
