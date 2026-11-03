/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_util.h"

#include <string.h>

#include <zephyr/ztest.h>

const char spec_config_json[] =
	"{\n"
	"  \"network\": { \"dhcp\": true, \"ip\": \"192.168.1.50\", \"netmask\": \"255.255.255.0\",\n"
	"               \"gateway\": \"192.168.1.1\", \"dns\": \"192.168.1.1\" },\n"
	"  \"ntp\":     { \"server\": \"pool.ntp.org\" },\n"
	"  \"mqtt\":    { \"host\": \"192.168.1.10\", \"port\": 1883, \"client_id\": \"\",\n"
	"               \"username\": \"\", \"password\": \"\", \"keepalive_s\": 30 },\n"
	"  \"sparkplug\": { \"group_id\": \"Plant1\", \"edge_node_id\": \"GW01\", "
	"\"primary_host_id\": \"\" },\n"
	"  \"rs485\":   { \"baud\": 9600, \"parity\": \"none\", \"stop_bits\": 1,\n"
	"               \"response_timeout_ms\": 200, \"retries\": 2, \"inter_frame_delay_ms\": 5 },\n"
	"  \"local_io\": { \"di_debounce_ms\": 20 },\n"
	"  \"devices\": [\n"
	"    {\n"
	"      \"name\": \"Meter1\", \"unit_id\": 1, \"poll_ms\": 1000, \"offline_after\": 3,\n"
	"      \"tags\": [\n"
	"        { \"name\": \"Voltage\", \"area\": \"input\",   \"address\": 0, \"type\": \"float32\",\n"
	"          \"word_order\": \"big\", \"scale\": 1.0, \"offset\": 0.0, \"deadband\": 0.1,\n"
	"          \"writable\": false, \"units\": \"V\" },\n"
	"        { \"name\": \"Setpoint\", \"area\": \"holding\", \"address\": 10, \"type\": \"int16\",\n"
	"          \"scale\": 0.1, \"writable\": true },\n"
	"        { \"name\": \"Run\",      \"area\": \"coil\",    \"address\": 0, \"type\": \"bool\",\n"
	"          \"writable\": true }\n"
	"      ]\n"
	"    }\n"
	"  ]\n"
	"}\n";

static struct gw_config cfg;
static char err[GW_CFG_ERR_MAX];

struct gw_config *test_config_load(const char *json)
{
	int ret = gw_config_parse(json, strlen(json), &cfg, err, sizeof(err));

	zassert_ok(ret, "unexpected parse error: %s", err);
	return &cfg;
}

const char *test_config_error(const char *json)
{
	int ret = gw_config_parse(json, strlen(json), &cfg, err, sizeof(err));

	zassert_equal(ret, -EINVAL, "document unexpectedly accepted: %s", json);
	return err;
}
