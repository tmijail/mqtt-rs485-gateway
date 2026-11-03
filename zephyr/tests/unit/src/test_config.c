/*
 * Configuration parsing and validation (CFG-1, SPEC section 6).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "gw_config.h"
#include "test_util.h"

#define MINIMAL_HEAD                                                                         \
	"{\"mqtt\":{\"host\":\"broker\"},\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":\"N\"}"

ZTEST(config, test_spec_example)
{
	const struct gw_config *c = test_config_load(spec_config_json);
	const struct gw_tag_cfg *t = c->tags;

	zassert_true(c->network.dhcp);
	zassert_mem_equal(c->network.ip, ((uint8_t[]){192, 168, 1, 50}), 4);
	zassert_true(c->network.has_dns);
	zassert_str_equal(c->ntp.server, "pool.ntp.org");
	zassert_str_equal(c->mqtt.host, "192.168.1.10");
	zassert_equal(c->mqtt.port, 1883);
	zassert_str_equal(c->mqtt.client_id, "GW01", "empty client_id defaults to edge_node_id");
	zassert_equal(c->mqtt.keepalive_s, 30);
	zassert_str_equal(c->sparkplug.group_id, "Plant1");
	zassert_equal(c->rs485.baud, 9600);
	zassert_equal(c->rs485.parity, GW_PARITY_NONE);
	zassert_equal(c->rs485.retries, 2);
	zassert_equal(c->local_io.di_debounce_ms, 20);

	zassert_equal(c->device_count, 1);
	zassert_equal(c->tag_count, 3);
	zassert_str_equal(c->devices[0].name, "Meter1");
	zassert_equal(c->devices[0].unit_id, 1);
	zassert_equal(c->devices[0].first_tag, 0);
	zassert_equal(c->devices[0].tag_count, 3);

	zassert_str_equal(t[0].name, "Voltage");
	zassert_equal(t[0].area, GW_AREA_INPUT);
	zassert_equal(t[0].type, GW_TYPE_FLOAT32);
	zassert_within(t[0].deadband, 0.1, 1e-12);
	zassert_equal(t[1].type, GW_TYPE_INT16);
	zassert_equal(t[1].word_order, GW_WORD_BIG, "word_order defaults to big");
	zassert_within(t[1].scale, 0.1, 1e-12);
	zassert_true(t[1].writable);
	zassert_equal(t[2].area, GW_AREA_COIL);
	zassert_equal(t[2].type, GW_TYPE_BOOL);
}

ZTEST(config, test_defaults)
{
	const struct gw_config *c = test_config_load(MINIMAL_HEAD "}");

	zassert_true(c->network.dhcp);
	zassert_str_equal(c->ntp.server, "pool.ntp.org");
	zassert_equal(c->mqtt.port, 1883);
	zassert_equal(c->mqtt.keepalive_s, 30);
	zassert_equal(c->rs485.baud, 9600);
	zassert_equal(c->rs485.stop_bits, 1);
	zassert_equal(c->rs485.response_timeout_ms, 200);
	zassert_equal(c->rs485.inter_frame_delay_ms, 5);
	zassert_equal(c->local_io.di_debounce_ms, 20);
	zassert_equal(c->device_count, 0);
}

ZTEST(config, test_static_ip_and_options)
{
	const struct gw_config *c = test_config_load(
		"{\"network\":{\"dhcp\":false,\"ip\":\"10.0.0.2\",\"netmask\":\"255.0.0.0\","
		"\"gateway\":\"10.0.0.1\"},"
		"\"mqtt\":{\"host\":\"b\",\"client_id\":\"cid\",\"username\":\"u\",\"password\":"
		"\"p\\\"w\"},"
		"\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":\"N\"},"
		"\"rs485\":{\"baud\":19200,\"parity\":\"even\",\"stop_bits\":2},"
		"\"devices\":[{\"name\":\"M\\u00e9ter\",\"unit_id\":7,\"tags\":["
		"{\"name\":\"E\",\"area\":\"holding\",\"address\":65534,\"type\":\"uint32\","
		"\"word_order\":\"little\"},"
		"{\"name\":\"D\",\"area\":\"discrete\",\"address\":3}]}]}");

	zassert_false(c->network.dhcp);
	zassert_false(c->network.has_dns);
	zassert_mem_equal(c->network.gateway, ((uint8_t[]){10, 0, 0, 1}), 4);
	zassert_str_equal(c->mqtt.client_id, "cid");
	zassert_str_equal(c->mqtt.password, "p\"w", "escapes are decoded");
	zassert_equal(c->rs485.parity, GW_PARITY_EVEN);
	zassert_equal(c->rs485.stop_bits, 2);
	zassert_str_equal(c->devices[0].name, "M\xc3\xa9ter", "\\u escapes become UTF-8");
	zassert_equal(c->tags[0].word_order, GW_WORD_LITTLE);
	zassert_equal(c->tags[1].type, GW_TYPE_BOOL, "bit areas default to bool");
}

struct error_case {
	const char *json;
	const char *expect;
};

ZTEST(config, test_descriptive_errors)
{
	static const struct error_case cases[] = {
		{"", "(root): expected an object, got nothing"},
		{"{\"mqtt\":{\"host\":\"b\",}}", "mqtt: line 1, col 21: trailing comma"},
		{"{\"mqtt\":{\"host\":\"b\"}\n\"x\":1}", "line 2, col 1: expected ',' or '}'"},
		{MINIMAL_HEAD "} x", "unexpected data after the document"},
		{"{\"mqtt\":{\"hots\":\"b\"}}", "mqtt.hots: unknown member"},
		{"{\"foo\":{}}", "foo: unknown member"},
		{"{\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":\"N\"}}",
		 "mqtt: section is required"},
		{"{\"mqtt\":{\"host\":\"\"}}", "mqtt.host: must not be empty"},
		{"{\"mqtt\":{\"host\":\"b\",\"port\":70000}}",
		 "mqtt.port: must be between 1 and 65535"},
		{"{\"mqtt\":{\"host\":\"b\",\"port\":\"1883\"}}",
		 "mqtt.port: expected a number, got a string"},
		{"{\"rs485\":{\"baud\":9600.5}}", "rs485.baud: expected an integer"},
		{"{\"rs485\":{\"parity\":\"mark\"}}", "rs485.parity: invalid value 'mark'"},
		{"{\"rs485\":{\"stop_bits\":3}}", "rs485.stop_bits: must be between 1 and 2"},
		{"{\"network\":{\"dhcp\":false,\"ip\":\"10.0.0.2\"}}",
		 "network: 'ip', 'netmask' and 'gateway' are required when dhcp is false"},
		{"{\"network\":{\"ip\":\"192.168.1.300\"}}",
		 "network.ip: invalid IPv4 address '192.168.1.300'"},
		{"{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":"
		 "\"a/b\"}}",
		 "sparkplug.edge_node_id: must not contain '/', '+' or '#'"},
		{MINIMAL_HEAD ",\"devices\":[{\"name\":\"D\",\"unit_id\":1,\"tags\":[{\"name\":\"T\","
			      "\"area\":\"holding\",\"type\":\"u16\"}]}]}",
		 "devices[0].tags[0].type: invalid value 'u16'"},
		{MINIMAL_HEAD ",\"devices\":[{\"name\":\"D\",\"unit_id\":1,\"tags\":[{\"name\":\"T\","
			      "\"area\":\"holding\",\"type\":\"bool\"}]}]}",
		 "devices[0].tags[0].type: 'bool' is the only type for coil/discrete"},
		{MINIMAL_HEAD ",\"devices\":[{\"name\":\"D\",\"unit_id\":1,\"tags\":[{\"name\":\"T\","
			      "\"area\":\"coil\",\"type\":\"uint16\"}]}]}",
		 "devices[0].tags[0].type: 'bool' is the only type"},
		{MINIMAL_HEAD ",\"devices\":[{\"name\":\"D\",\"unit_id\":1,\"tags\":[{\"name\":\"T\","
			      "\"area\":\"input\",\"writable\":true}]}]}",
		 "devices[0].tags[0].writable: only coil and holding tags can be writable"},
		{MINIMAL_HEAD ",\"devices\":[{\"name\":\"D\",\"unit_id\":1,\"tags\":[{\"name\":\"T\","
			      "\"area\":\"holding\",\"scale\":0}]}]}",
		 "devices[0].tags[0].scale: must not be zero"},
		{MINIMAL_HEAD ",\"devices\":[{\"name\":\"D\",\"unit_id\":1,\"tags\":[{\"name\":\"T\","
			      "\"area\":\"holding\",\"type\":\"uint32\",\"address\":65535}]}]}",
		 "devices[0].tags[0].address: register range exceeds 65535"},
		{MINIMAL_HEAD ",\"devices\":[{\"name\":\"D\",\"unit_id\":1,\"tags\":[{\"name\":\"T\","
			      "\"area\":\"holding\"},{\"name\":\"T\",\"area\":\"input\"}]}]}",
		 "devices[0].tags[1].name: duplicate tag name 'T'"},
		{MINIMAL_HEAD ",\"devices\":[{\"name\":\"D\",\"unit_id\":1,\"tags\":[{\"name\":\"T\","
			      "\"area\":\"holding\"}]},{\"name\":\"E\",\"unit_id\":1,\"tags\":[{\"name\":"
			      "\"T\",\"area\":\"holding\"}]}]}",
		 "devices[1].unit_id: unit_id 1 already used by 'D'"},
		{MINIMAL_HEAD ",\"devices\":[{\"name\":\"D\",\"unit_id\":1,\"tags\":[]}]}",
		 "devices[0].tags: at least one tag is required"},
		{MINIMAL_HEAD ",\"devices\":[{\"name\":\"D\",\"tags\":[{\"name\":\"T\","
			      "\"area\":\"holding\"}]}]}",
		 "devices[0].unit_id: is required"},
		{MINIMAL_HEAD ",\"devices\":[{\"name\":\"D\",\"unit_id\":1,\"tags\":[{\"name\":\"T\""
			      "}]}]}",
		 "devices[0].tags[0].area: is required"},
		{MINIMAL_HEAD ",\"devices\":[{\"name\":\"0123456789012345678901234567890123\"}]}",
		 "devices[0].name: line 1, col 128: string too long (max 32 bytes)"},
		{"{\"mqtt\":{\"host\":\"\\q\"}}", "invalid escape sequence"},
		{"{\"mqtt\":{\"host\":\"\\ud800\"}}", "unpaired UTF-16 surrogate"},
		{"{\"rs485\":{\"retries\":01}}", "rs485: line 1, col 22: expected ',' or '}'"},
		{"{\"rs485\":{\"baud\":-}}", "rs485.baud: line 1, col 18: invalid number"},
	};

	int failures = 0;

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		const char *err = test_config_error(cases[i].json);

		if (strstr(err, cases[i].expect) == NULL) {
			printk("case %u: got \"%s\", want \"%s\"\n", (unsigned int)i, err,
			       cases[i].expect);
			failures++;
		}
	}
	zassert_equal(failures, 0, "%d error message(s) differ", failures);
}

/* Build a document with @p devices devices of @p tags tags each. */
static const char *build_doc(int devices, int tags)
{
	static char doc[96 * 1024];
	size_t n = 0;

	n += snprintf(&doc[n], sizeof(doc) - n, MINIMAL_HEAD ",\"devices\":[");
	for (int d = 0; d < devices; d++) {
		n += snprintf(&doc[n], sizeof(doc) - n, "%s{\"name\":\"D%d\",\"unit_id\":%d,\"tags\":[",
			      d ? "," : "", d, d + 1);
		for (int t = 0; t < tags; t++) {
			n += snprintf(&doc[n], sizeof(doc) - n,
				      "%s{\"name\":\"T%d\",\"area\":\"holding\",\"address\":%d}",
				      t ? "," : "", t, t);
		}
		n += snprintf(&doc[n], sizeof(doc) - n, "]}");
	}
	snprintf(&doc[n], sizeof(doc) - n, "]}");
	return doc;
}

ZTEST(config, test_limits)
{
	const struct gw_config *c;

	c = test_config_load(build_doc(4, CONFIG_GW_MAX_TAGS_PER_DEVICE));
	zassert_equal(c->tag_count, CONFIG_GW_MAX_TAGS, "exactly at the total limit");
	zassert_equal(c->devices[3].first_tag, 3 * CONFIG_GW_MAX_TAGS_PER_DEVICE);

	zassert_not_null(strstr(test_config_error(build_doc(1, CONFIG_GW_MAX_TAGS_PER_DEVICE + 1)),
				"devices[0].tags: too many tags (max 64 per device)"));
	zassert_not_null(strstr(test_config_error(build_doc(5, CONFIG_GW_MAX_TAGS_PER_DEVICE)),
				"devices[4].tags: too many tags (max 256 in total)"));
	zassert_not_null(strstr(test_config_error(build_doc(CONFIG_GW_MAX_DEVICES + 1, 1)),
				"too many devices (max 16)"));
}

ZTEST_SUITE(config, NULL, NULL, NULL, NULL, NULL);
