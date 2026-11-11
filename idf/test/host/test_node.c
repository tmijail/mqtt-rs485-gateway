/* QA-1: Sparkplug edge-node behaviour: births, deaths, seq, aliases, report by
 * exception, rebirth and NCMD/DCMD handling (SPB-2..SPB-12). */
#include <stdio.h>
#include <string.h>

#include "gw_mbap.h"
#include "gw_node.h"
#include "gw_port.h"
#include "gw_tagdb.h"
#include "spb.h"
#include "support.h"
#include "unity.h"

static const gw_config_t *cfg;
static gw_node_t node;
static decoded_t dec;
static uint8_t cmd[512];

#define VOLTAGE 0
#define SETPOINT 1
#define RUN 2

void setUp(void)
{
    cfg = support_spec_config();
    gw_tagdb_reset();
    recorder_clear();
    recorder_default_diag();
    gw_node_ops_t ops = recorder_ops();
    const gw_node_info_t info = {
        .firmware = "0.1.0+gdeadbee", .hardware = "test-hw", .reset_reason = "task watchdog"};
    gw_node_init(&node, cfg, &ops, &info);
    support_node_buffer(&node);
}
void tearDown(void) {}

static void set_meter(bool online, double voltage, double setpoint, double run)
{
    gw_tag_value_t v[3] = {
        {.value = voltage, .ts_ms = 1000, .valid = true},
        {.value = setpoint, .ts_ms = 1000, .valid = true},
        {.value = run, .ts_ms = 1000, .valid = true},
    };
    gw_tagdb_put_device(cfg, 0, v, online);
}

static const rec_msg_t *msg(size_t i)
{
    TEST_ASSERT_TRUE_MESSAGE(i < g_rec.count, "message not published");
    TEST_ASSERT_TRUE_MESSAGE(decode_payload(g_rec.msgs[i].payload, g_rec.msgs[i].len, &dec),
                             "payload does not decode");
    return &g_rec.msgs[i];
}

static size_t encode_cmd(const spb_metric_t *m, size_t n)
{
    spb_payload_t p = {.timestamp = 1, .has_seq = false, .metrics = m, .metric_count = n};
    size_t len = spb_encode(&p, cmd, sizeof(cmd));
    TEST_ASSERT_GREATER_THAN(0, len);
    return len;
}

static void send(const char *topic, const spb_metric_t *m, size_t n)
{
    size_t len = encode_cmd(m, n);
    gw_node_handle_message(&node, topic, strlen(topic), cmd, len);
}

static void send_raw(const char *topic, const raw_metric_t *m, size_t n)
{
    size_t len = raw_payload(cmd, sizeof(cmd), m, n);
    gw_node_handle_message(&node, topic, strlen(topic), cmd, len);
}

static void test_nbirth_and_dbirth(void)
{
    set_meter(true, 230.5, 12.3, 1);
    gw_tagdb_set_di(0x05, 500);
    gw_node_session_up(&node, 7);
    TEST_ASSERT_EQUAL(2, g_rec.count);

    const rec_msg_t *nb = msg(0);
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/NBIRTH/GW01", nb->topic);
    TEST_ASSERT_EQUAL(0, nb->qos);
    TEST_ASSERT_FALSE(nb->retain);
    TEST_ASSERT_TRUE(dec.has_seq);
    TEST_ASSERT_EQUAL_UINT64(0, dec.seq);
    TEST_ASSERT_EQUAL(GW_NODE_BIRTH_METRICS - 1, dec.count);   /* no Modbus TCP table */
    int i = decoded_find_name(&dec, "bdSeq");
    TEST_ASSERT_TRUE(i >= 0);
    /* tck-id-message-flow-edge-node-birth-publish-nbirth-payload-bdSeq: the NBIRTH carries
     * bdSeq with the datatype INT64 (4), not UInt64 (#154) */
    TEST_ASSERT_EQUAL(SPB_DT_INT64, dec.m[i].datatype);
    TEST_ASSERT_EQUAL(SPB_VAL_LONG, dec.m[i].kind);
    TEST_ASSERT_EQUAL_DOUBLE(7, dec.m[i].num);
    i = decoded_find_name(&dec, "Node Control/Rebirth");
    TEST_ASSERT_TRUE(i >= 0);
    TEST_ASSERT_EQUAL(SPB_DT_BOOLEAN, dec.m[i].datatype);
    TEST_ASSERT_EQUAL_DOUBLE(0, dec.m[i].num);   /* tck-id-payloads-nbirth-rebirth-req */
    /* tck-id-operational-behavior-data-commands-rebirth-name-aliases: no alias (#153) */
    TEST_ASSERT_FALSE(dec.m[i].has_alias);
    TEST_ASSERT_TRUE(decoded_find_name(&dec, "Node Control/Reboot") >= 0);
    i = decoded_find_name(&dec, "Node Info/Firmware");
    TEST_ASSERT_EQUAL_STRING("0.1.0+gdeadbee", dec.m[i].str);
    TEST_ASSERT_TRUE(decoded_find_name(&dec, "Node Info/Hardware") >= 0);
    i = decoded_find_name(&dec, "Node Info/Reset Reason");
    TEST_ASSERT_TRUE(i >= 0);
    TEST_ASSERT_EQUAL(SPB_DT_STRING, dec.m[i].datatype);
    TEST_ASSERT_EQUAL_UINT64(GW_ALIAS_RESET_REASON, dec.m[i].alias);
    TEST_ASSERT_EQUAL_STRING("task watchdog", dec.m[i].str);
    i = decoded_find_name(&dec, "DI/1");
    TEST_ASSERT_EQUAL_DOUBLE(1, dec.m[i].num);
    TEST_ASSERT_EQUAL_UINT64(500, dec.m[i].timestamp);
    i = decoded_find_name(&dec, "DI/2");
    TEST_ASSERT_EQUAL_DOUBLE(0, dec.m[i].num);
    TEST_ASSERT_TRUE(decoded_find_name(&dec, "DO/8") >= 0);
    /* every birth metric has name, alias (except Rebirth) and datatype; aliases are unique */
    for (size_t a = 0; a < dec.count; a++) {
        TEST_ASSERT_TRUE(dec.m[a].has_name);
        TEST_ASSERT_TRUE(dec.m[a].has_datatype);   /* tck-id-payloads-metric-datatype-req */
        TEST_ASSERT_NOT_EQUAL(0, dec.m[a].datatype);
        TEST_ASSERT_TRUE(dec.m[a].has_alias || strcmp(dec.m[a].name, "Node Control/Rebirth") == 0);
        for (size_t b = a + 1; b < dec.count; b++) {
            TEST_ASSERT_TRUE(!dec.m[a].has_alias || !dec.m[b].has_alias ||
                             dec.m[a].alias != dec.m[b].alias);
        }
    }

    const rec_msg_t *db = msg(1);
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/DBIRTH/GW01/Meter1", db->topic);
    TEST_ASSERT_EQUAL_UINT64(1, dec.seq);
    TEST_ASSERT_EQUAL(3, dec.count);
    i = decoded_find_name(&dec, "Voltage");
    TEST_ASSERT_EQUAL(SPB_DT_FLOAT, dec.m[i].datatype);
    TEST_ASSERT_EQUAL_UINT64(GW_ALIAS_TAG_BASE + VOLTAGE, dec.m[i].alias);
    TEST_ASSERT_EQUAL_DOUBLE(230.5, dec.m[i].num);
    i = decoded_find_name(&dec, "Setpoint");
    TEST_ASSERT_EQUAL(SPB_DT_FLOAT, dec.m[i].datatype);  /* int16 with scale 0.1 */
    TEST_ASSERT_DOUBLE_WITHIN(1e-5, 12.3, dec.m[i].num);
    i = decoded_find_name(&dec, "Run");
    TEST_ASSERT_EQUAL(SPB_DT_BOOLEAN, dec.m[i].datatype);
    for (size_t a = 0; a < dec.count; a++) {
        TEST_ASSERT_TRUE(dec.m[a].has_datatype);   /* tck-id-payloads-metric-datatype-req */
    }
}

static void test_offline_device_has_no_dbirth_until_online(void)
{
    set_meter(false, 0, 0, 0);
    gw_node_session_up(&node, 0);
    TEST_ASSERT_EQUAL(1, g_rec.count);
    gw_node_device_changed(&node, 0);  /* still offline: nothing */
    TEST_ASSERT_EQUAL(1, g_rec.count);
    set_meter(true, 1, 2, 0);
    gw_node_device_changed(&node, 0);
    msg(1);
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/DBIRTH/GW01/Meter1", g_rec.msgs[1].topic);
    set_meter(false, 1, 2, 0);
    gw_node_device_changed(&node, 0);
    msg(2);
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/DDEATH/GW01/Meter1", g_rec.msgs[2].topic);
    TEST_ASSERT_EQUAL(0, dec.count);
    TEST_ASSERT_EQUAL_UINT64(2, dec.seq);
    gw_node_device_changed(&node, 0);  /* no second DDEATH */
    TEST_ASSERT_EQUAL(3, g_rec.count);
}

static void test_ddata_by_exception_with_deadband(void)
{
    set_meter(true, 230.0, 12.3, 0);
    gw_node_session_up(&node, 0);
    recorder_clear();
    set_meter(true, 230.05, 12.3, 0);  /* inside the 0.1 V deadband */
    gw_node_device_changed(&node, 0);
    TEST_ASSERT_EQUAL(0, g_rec.count);
    set_meter(true, 230.2, 12.3, 1);   /* voltage and run changed in the same poll cycle */
    gw_node_device_changed(&node, 0);
    TEST_ASSERT_EQUAL(1, g_rec.count);
    const rec_msg_t *m = msg(0);
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/DDATA/GW01/Meter1", m->topic);
    TEST_ASSERT_EQUAL(2, dec.count);   /* one message with both changes */
    for (size_t i = 0; i < dec.count; i++) {
        TEST_ASSERT_FALSE(dec.m[i].has_name);      /* alias only */
        TEST_ASSERT_FALSE(dec.m[i].has_datatype);  /* tck-id-payloads-metric-datatype-not-req */
        TEST_ASSERT_EQUAL_UINT64(1000, dec.m[i].timestamp);
    }
    /* the value is in the field of the DBIRTH datatype: Float for Voltage, Boolean for Run */
    int volt = decoded_find_alias(&dec, GW_ALIAS_TAG_BASE + VOLTAGE);
    TEST_ASSERT_EQUAL(SPB_VAL_FLOAT, dec.m[volt].kind);
    TEST_ASSERT_EQUAL(SPB_VAL_BOOL, dec.m[decoded_find_alias(&dec, GW_ALIAS_TAG_BASE + RUN)].kind);
    int v = decoded_find_alias(&dec, GW_ALIAS_TAG_BASE + VOLTAGE);
    TEST_ASSERT_DOUBLE_WITHIN(1e-4, 230.2, dec.m[v].num);
    TEST_ASSERT_TRUE(decoded_find_alias(&dec, GW_ALIAS_TAG_BASE + RUN) >= 0);
    TEST_ASSERT_EQUAL_UINT64(2, dec.seq);  /* NBIRTH 0, DBIRTH 1, DDATA 2 */

    /* a failed read is reported as null, once */
    gw_tag_value_t bad = {.value = 0, .ts_ms = 2000, .valid = false};
    gw_tagdb_put_tag(VOLTAGE, &bad);
    recorder_clear();
    gw_node_device_changed(&node, 0);
    gw_node_device_changed(&node, 0);
    TEST_ASSERT_EQUAL(1, g_rec.count);
    msg(0);
    TEST_ASSERT_TRUE(dec.m[0].is_null);
}

static void test_seq_wraps_and_is_shared(void)
{
    set_meter(true, 0, 0, 0);
    gw_node_session_up(&node, 0);
    for (int i = 1; i <= 300; i++) {
        recorder_clear();   /* the recorder holds REC_MAX messages; a full one fails (#158) */
        gw_tagdb_set_di((uint8_t)(i & 1), (uint64_t)i);
        gw_node_io_changed(&node);
        TEST_ASSERT_EQUAL(1, g_rec.count);
        msg(0);
        TEST_ASSERT_EQUAL_UINT64((uint64_t)((i + 1) % 256), dec.seq);
    }
    /* 2 births + 300 NDATA, seq continues 0..255,0.. */
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(302 % 256), node.seq);
}

static void test_ncmd_rebirth(void)
{
    set_meter(true, 1, 2, 0);
    gw_node_session_up(&node, 3);
    gw_tagdb_set_di(0x01, 10);
    gw_node_io_changed(&node);
    recorder_clear();
    spb_metric_t m = {.name = "Node Control/Rebirth", .alias = 0, .datatype = SPB_DT_BOOLEAN,
                      .v.num = 1};
    send("spBv1.0/Plant1/NCMD/GW01", &m, 1);
    TEST_ASSERT_EQUAL(2, g_rec.count);
    msg(0);
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/NBIRTH/GW01", g_rec.msgs[0].topic);
    TEST_ASSERT_EQUAL_UINT64(0, dec.seq);
    TEST_ASSERT_EQUAL_DOUBLE(3, dec.m[decoded_find_name(&dec, "bdSeq")].num); /* same session */
    /* the request came by name: the NBIRTH has no alias for Rebirth (#153) */
    TEST_ASSERT_FALSE(dec.m[decoded_find_name(&dec, "Node Control/Rebirth")].has_alias);
    msg(1);
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/DBIRTH/GW01/Meter1", g_rec.msgs[1].topic);
    TEST_ASSERT_EQUAL_UINT64(1, dec.seq);

    /* by alias, and false is ignored */
    recorder_clear();
    spb_metric_t f = {.alias = GW_ALIAS_REBIRTH, .datatype = SPB_DT_BOOLEAN, .v.num = 0};
    send("spBv1.0/Plant1/NCMD/GW01", &f, 1);
    TEST_ASSERT_EQUAL(0, g_rec.count);
    /* another node's NCMD is ignored */
    send("spBv1.0/Plant1/NCMD/GW02", &m, 1);
    TEST_ASSERT_EQUAL(0, g_rec.count);
}

/* tck-id-payloads-name-birth-data-requirement (#166): every metric of NBIRTH, DBIRTH and DATA
 * has a timestamp; one with no sample time (0) gets the time of the message. */
static void test_every_metric_has_a_timestamp(void)
{
    set_meter(true, 1, 2, 0);
    gw_node_session_up(&node, 0);
    TEST_ASSERT_EQUAL(2, g_rec.count);
    for (size_t k = 0; k < 2; k++) {
        msg(k);
        for (size_t i = 0; i < dec.count; i++) {
            TEST_ASSERT_NOT_EQUAL_UINT64(0, dec.m[i].timestamp);
        }
    }
    recorder_clear();
    gw_tagdb_set_do_unknown(0);   /* the expander is lost: no sample time for the DOs */
    gw_node_io_changed(&node);
    msg(0);
    TEST_ASSERT_EQUAL(GW_NODE_IO_COUNT, dec.count);
    for (size_t i = 0; i < dec.count; i++) {
        TEST_ASSERT_TRUE(dec.m[i].is_null);
        TEST_ASSERT_NOT_EQUAL_UINT64(0, dec.m[i].timestamp);
        TEST_ASSERT_FALSE(dec.m[i].has_datatype);  /* NDATA: no datatype (#159) */
    }
}

static void test_ncmd_do_and_reboot(void)
{
    set_meter(true, 1, 2, 0);
    gw_node_session_up(&node, 9);
    recorder_clear();
    spb_metric_t m = {.name = "DO/3", .datatype = SPB_DT_BOOLEAN, .v.num = 1};
    send("spBv1.0/Plant1/NCMD/GW01", &m, 1);
    TEST_ASSERT_EQUAL_HEX8(0x04, g_rec.dout);
    TEST_ASSERT_EQUAL(1, g_rec.count);
    msg(0);
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/NDATA/GW01", g_rec.msgs[0].topic);
    TEST_ASSERT_EQUAL(1, dec.count);
    TEST_ASSERT_EQUAL_UINT64(GW_ALIAS_DO_BASE + 2, dec.m[0].alias);
    TEST_ASSERT_EQUAL_DOUBLE(1, dec.m[0].num);

    /* repeating the same command still confirms the state */
    recorder_clear();
    spb_metric_t a = {.alias = GW_ALIAS_DO_BASE + 2, .datatype = SPB_DT_BOOLEAN, .v.num = 1};
    send("spBv1.0/Plant1/NCMD/GW01", &a, 1);
    TEST_ASSERT_EQUAL(1, g_rec.count);

    /* read-only and unknown metrics are discarded */
    recorder_clear();
    spb_metric_t ro[2] = {{.name = "DI/1", .datatype = SPB_DT_BOOLEAN, .v.num = 1},
                          {.name = "Nope", .datatype = SPB_DT_BOOLEAN, .v.num = 1}};
    send("spBv1.0/Plant1/NCMD/GW01", ro, 2);
    TEST_ASSERT_EQUAL(0, g_rec.count);
    gw_local_io_t io;
    gw_tagdb_get_io(&io);
    TEST_ASSERT_EQUAL_HEX8(0x04, io.dout);

    spb_metric_t rb = {.name = "Node Control/Reboot", .datatype = SPB_DT_BOOLEAN, .v.num = 1};
    send("spBv1.0/Plant1/NCMD/GW01", &rb, 1);
    TEST_ASSERT_EQUAL(1, g_rec.reboots);
    const rec_msg_t *d = msg(0);
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/NDEATH/GW01", d->topic);
    TEST_ASSERT_EQUAL(1, d->qos);
    TEST_ASSERT_FALSE(dec.has_seq);
    TEST_ASSERT_EQUAL_DOUBLE(9, dec.m[0].num);
}

/* IO-4: each DI activation counts one pulse; the counters are UInt64 metrics in NBIRTH and
 * go in the same NDATA as the DI change that made them. */
static void test_pulse_counters(void)
{
    const uint64_t restored[8] = {0, 5, 0, 0, 0, 0, 0, (1ull << 53) + 1};
    gw_tagdb_set_counters(restored);
    gw_tagdb_set_di(0x01, 100);                 /* start-up state: not a pulse */
    gw_node_session_up(&node, 0);
    msg(0);
    int i = decoded_find_name(&dec, "Counter/2");
    TEST_ASSERT_TRUE(i >= 0);
    TEST_ASSERT_EQUAL(SPB_DT_UINT64, dec.m[i].datatype);
    TEST_ASSERT_EQUAL_UINT64(GW_ALIAS_COUNT_BASE + 1, dec.m[i].alias);
    TEST_ASSERT_EQUAL_UINT64(5, dec.m[i].u64);
    i = decoded_find_name(&dec, "Counter/1");
    TEST_ASSERT_EQUAL_UINT64(0, dec.m[i].u64);
    i = decoded_find_name(&dec, "Counter/8");
    TEST_ASSERT_EQUAL_UINT64((1ull << 53) + 1, dec.m[i].u64);   /* exact */

    recorder_clear();
    gw_tagdb_update_di(0x03, 200);              /* DI/2 activated */
    gw_node_io_changed(&node);
    msg(0);
    TEST_ASSERT_EQUAL(2, dec.count);           /* DI/2 and Counter/2 */
    i = decoded_find_alias(&dec, GW_ALIAS_COUNT_BASE + 1);
    TEST_ASSERT_TRUE(i >= 0);
    TEST_ASSERT_EQUAL_UINT64(6, dec.m[i].u64);
    TEST_ASSERT_EQUAL_UINT64(200, dec.m[i].timestamp);
    TEST_ASSERT_FALSE(dec.m[i].has_name);

    recorder_clear();
    gw_tagdb_update_di(0x00, 300);              /* releases do not count */
    gw_node_io_changed(&node);
    msg(0);
    TEST_ASSERT_EQUAL(2, dec.count);           /* DI/1 and DI/2 only */
    TEST_ASSERT_EQUAL(-1, decoded_find_alias(&dec, GW_ALIAS_COUNT_BASE));
    gw_tagdb_update_di(0x81, 400);
    gw_local_io_t io;
    gw_tagdb_get_io(&io);
    TEST_ASSERT_EQUAL_UINT64(1, io.count[0]);
    TEST_ASSERT_EQUAL_UINT64((1ull << 53) + 2, io.count[7]);
}

static void test_dcmd_writes(void)
{
    set_meter(true, 1, 2, 0);
    gw_node_session_up(&node, 0);
    recorder_clear();

    spb_metric_t sp = {.name = "Setpoint", .datatype = SPB_DT_FLOAT, .v.num = 12.3};
    send("spBv1.0/Plant1/DCMD/GW01/Meter1", &sp, 1);
    TEST_ASSERT_EQUAL(1, g_rec.writes);
    TEST_ASSERT_EQUAL(SETPOINT, g_rec.last_write_tag);
    TEST_ASSERT_DOUBLE_WITHIN(1e-5, 12.3, g_rec.last_write_value);

    spb_metric_t run = {.alias = GW_ALIAS_TAG_BASE + RUN, .datatype = SPB_DT_BOOLEAN, .v.num = 1};
    send("spBv1.0/Plant1/DCMD/GW01/Meter1", &run, 1);
    TEST_ASSERT_EQUAL(2, g_rec.writes);
    TEST_ASSERT_EQUAL(RUN, g_rec.last_write_tag);

    /* read-only, unknown, foreign alias, unknown device: all discarded */
    spb_metric_t ro = {.name = "Voltage", .datatype = SPB_DT_FLOAT, .v.num = 1};
    send("spBv1.0/Plant1/DCMD/GW01/Meter1", &ro, 1);
    spb_metric_t unk = {.name = "Nope", .datatype = SPB_DT_FLOAT, .v.num = 1};
    send("spBv1.0/Plant1/DCMD/GW01/Meter1", &unk, 1);
    spb_metric_t foreign = {.alias = GW_ALIAS_DO_BASE, .datatype = SPB_DT_BOOLEAN, .v.num = 1};
    send("spBv1.0/Plant1/DCMD/GW01/Meter1", &foreign, 1);
    send("spBv1.0/Plant1/DCMD/GW01/Meter2", &sp, 1);
    TEST_ASSERT_EQUAL(2, g_rec.writes);
    TEST_ASSERT_EQUAL(0, g_rec.count);

    /* confirmation after the RS485 task wrote and read back */
    gw_tag_value_t v = {.value = 12.3, .ts_ms = 3000, .valid = true};
    gw_tagdb_put_tag(SETPOINT, &v);
    gw_node_write_done(&node, SETPOINT, true);
    TEST_ASSERT_EQUAL(1, g_rec.count);
    msg(0);
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/DDATA/GW01/Meter1", g_rec.msgs[0].topic);
    TEST_ASSERT_EQUAL_UINT64(GW_ALIAS_TAG_BASE + SETPOINT, dec.m[0].alias);
    TEST_ASSERT_DOUBLE_WITHIN(1e-5, 12.3, dec.m[0].num);
    gw_node_write_done(&node, SETPOINT, false);
    TEST_ASSERT_EQUAL(1, g_rec.count);
    gw_node_write_done(&node, 3, true);   /* no such tag: nothing is attributed to Meter1 */
    TEST_ASSERT_EQUAL(1, g_rec.count);

    /* offline device: DCMD discarded */
    set_meter(false, 0, 0, 0);
    gw_node_device_changed(&node, 0);
    send("spBv1.0/Plant1/DCMD/GW01/Meter1", &sp, 1);
    TEST_ASSERT_EQUAL(2, g_rec.writes);
}

/* SPB-10 (#163): a DCMD metric with a datatype other than the one of the DBIRTH, or with a
 * fraction for an integer or Boolean tag, is rejected with a log line: no write, no DDATA.
 * Without a datatype (the Sparkplug recommendation for commands,
 * tck-id-payloads-metric-datatype-not-req), the datatype of the DBIRTH applies. */
static void test_dcmd_strict_datatype_and_integer(void)
{
    static const char doc[] =
        "{\"mqtt\": {\"host\": \"h\"}, \"sparkplug\": {\"group_id\": \"G\", \"edge_node_id\": \"N\"},"
        " \"devices\": [{\"name\": \"M\", \"unit_id\": 1, \"tags\": ["
        "  {\"name\": \"Level\", \"area\": \"holding\", \"address\": 0, \"type\": \"int16\","
        "   \"writable\": true},"
        "  {\"name\": \"Gain\", \"area\": \"holding\", \"address\": 1, \"type\": \"int16\","
        "   \"scale\": 0.1, \"writable\": true},"
        "  {\"name\": \"Run\", \"area\": \"coil\", \"address\": 0, \"type\": \"bool\","
        "   \"writable\": true}]}]}";
    static gw_config_t c;
    gw_cfg_err_t err;
    TEST_ASSERT_TRUE_MESSAGE(gw_config_parse(doc, sizeof(doc) - 1, &c, &err), err.msg);
    cfg = &c;
    gw_tagdb_reset();
    gw_node_ops_t ops = recorder_ops();
    const gw_node_info_t info = {.firmware = "f", .hardware = "h", .reset_reason = "r"};
    gw_node_init(&node, cfg, &ops, &info);
    support_node_buffer(&node);
    gw_tag_value_t v[3] = {{.value = 1, .ts_ms = 1000, .valid = true},
                           {.value = 1, .ts_ms = 1000, .valid = true},
                           {.value = 0, .ts_ms = 1000, .valid = true}};
    gw_tagdb_put_device(cfg, 0, v, true);
    gw_node_session_up(&node, 0);
    recorder_clear();
    const char *topic = "spBv1.0/G/DCMD/N/M";
    static char log[2048];
    gw_port_log_capture(log, sizeof(log));

    /* another datatype: Double for the Int16 tag, Int16 for the Float (scaled) tag */
    raw_metric_t dbl = {.name = "Level", .datatype = SPB_DT_DOUBLE, .kind = RAW_DOUBLE, .num = 7};
    send_raw(topic, &dbl, 1);
    TEST_ASSERT_NOT_NULL(strstr(log, "metric 'Level' discarded: datatype 10 is not the datatype 2 "
                                     "of the DBIRTH"));
    spb_metric_t i16 = {.name = "Gain", .no_alias = true, .datatype = SPB_DT_INT16, .v.num = 5};
    send(topic, &i16, 1);
    TEST_ASSERT_NOT_NULL(strstr(log, "metric 'Gain' discarded: datatype 2 is not the datatype 9"));
    /* a fraction for the Int16 tag: with its datatype, and without a datatype */
    raw_metric_t frac = {.name = "Level", .datatype = SPB_DT_INT16, .kind = RAW_FLOAT, .num = 1.5};
    send_raw(topic, &frac, 1);
    frac.no_datatype = true;
    send_raw(topic, &frac, 1);
    TEST_ASSERT_NOT_NULL(strstr(log, "metric 'Level' discarded: value with a fraction for an "
                                     "integer tag"));
    /* a fraction for the Boolean tag */
    raw_metric_t half = {.name = "Run", .no_datatype = true, .kind = RAW_DOUBLE, .num = 0.5};
    send_raw(topic, &half, 1);
    TEST_ASSERT_NOT_NULL(strstr(log, "metric 'Run' discarded: value with a fraction for a "
                                     "Boolean tag"));
    TEST_ASSERT_EQUAL(0, g_rec.writes);
    TEST_ASSERT_EQUAL(0, g_rec.count);   /* no DDATA */

    /* accepted: the datatype of the DBIRTH or none, an integral value for Level, a fraction for
     * the scaled Gain */
    raw_metric_t ok = {.name = "Level", .no_datatype = true, .kind = RAW_DOUBLE, .num = -7};
    send_raw(topic, &ok, 1);
    TEST_ASSERT_EQUAL(1, g_rec.writes);
    TEST_ASSERT_EQUAL_DOUBLE(-7, g_rec.last_write.value);
    spb_metric_t lvl = {.name = "Level", .no_alias = true, .datatype = SPB_DT_INT16, .v.num = 12};
    send(topic, &lvl, 1);
    TEST_ASSERT_EQUAL(2, g_rec.writes);
    TEST_ASSERT_EQUAL_DOUBLE(12, g_rec.last_write.value);
    spb_metric_t gain = {.name = "Gain", .no_alias = true, .datatype = SPB_DT_FLOAT, .v.num = 0.5};
    send(topic, &gain, 1);
    TEST_ASSERT_EQUAL(3, g_rec.writes);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.5, g_rec.last_write.value);
    gw_port_log_capture(NULL, 0);
}

/* Issue #60: a DCMD whose writes do not all fit in the bus queue is rejected whole. */
static void test_dcmd_rejected_whole_when_queue_lacks_room(void)
{
    set_meter(true, 1, 2, 0);
    gw_node_session_up(&node, 0);
    recorder_clear();
    spb_metric_t two[3] = {
        {.name = "Setpoint", .datatype = SPB_DT_FLOAT, .v.num = 12.3},
        {.name = "Voltage", .datatype = SPB_DT_FLOAT, .v.num = 1},   /* read-only: not counted */
        {.alias = GW_ALIAS_TAG_BASE + RUN, .datatype = SPB_DT_BOOLEAN, .v.num = 1},
    };
    g_rec.room_limited = true;
    g_rec.room = 1;
    send("spBv1.0/Plant1/DCMD/GW01/Meter1", two, 3);
    TEST_ASSERT_EQUAL(0, g_rec.writes);      /* 2 writes, room for 1: nothing queued */
    TEST_ASSERT_EQUAL(1, g_rec.room);
    g_rec.room = 2;                          /* exactly enough */
    send("spBv1.0/Plant1/DCMD/GW01/Meter1", two, 3);
    TEST_ASSERT_EQUAL(2, g_rec.writes);
    TEST_ASSERT_EQUAL(RUN, g_rec.last_write_tag);
    TEST_ASSERT_EQUAL(0, g_rec.room);
    /* a single write with no room left is rejected too */
    send("spBv1.0/Plant1/DCMD/GW01/Meter1", &two[0], 1);
    TEST_ASSERT_EQUAL(2, g_rec.writes);
    TEST_ASSERT_EQUAL(0, g_rec.count);
}

/* ---- MB-6: Int64 / UInt64 / Double / String metrics ---- */

static const char k_mb6[] =
    "{\"mqtt\": {\"host\": \"h\"}, \"sparkplug\": {\"group_id\": \"G\", \"edge_node_id\": \"N\"},"
    " \"devices\": [{\"name\": \"M\", \"unit_id\": 1, \"tags\": ["
    "  {\"name\": \"U64\", \"area\": \"holding\", \"address\": 0, \"type\": \"uint64\","
    "   \"writable\": true},"
    "  {\"name\": \"I64\", \"area\": \"holding\", \"address\": 4, \"type\": \"int64\","
    "   \"writable\": true, \"deadband\": 5},"
    "  {\"name\": \"F64\", \"area\": \"input\", \"address\": 0, \"type\": \"float64\"},"
    "  {\"name\": \"Str\", \"area\": \"holding\", \"address\": 10, \"type\": \"string\","
    "   \"length\": 2, \"writable\": true},"
    "  {\"name\": \"Bit\", \"area\": \"holding\", \"address\": 20, \"type\": \"bool\", \"bit\": 2,"
    "   \"writable\": true}"
    "]}]}";

static gw_config_t mb6;

static void put_mb6(uint64_t u64, int64_t i64, double f64, const char *str, bool bit)
{
    gw_tag_value_t v[5];
    memset(v, 0, sizeof(v));
    v[0].u64 = u64;
    v[1].i64 = i64;
    v[2].value = f64;
    strcpy(v[3].str, str);
    v[4].value = bit ? 1.0 : 0.0;
    for (int i = 0; i < 5; i++) {
        v[i].ts_ms = 1000;
        v[i].valid = true;
    }
    gw_tagdb_put_device(&mb6, 0, v, true);
}

static void setup_mb6(void)
{
    gw_cfg_err_t err;
    TEST_ASSERT_TRUE_MESSAGE(gw_config_parse(k_mb6, sizeof(k_mb6) - 1, &mb6, &err), err.msg);
    cfg = &mb6;
    gw_tagdb_reset();
    recorder_clear();
    gw_node_ops_t ops = recorder_ops();
    const gw_node_info_t info = {.firmware = "f", .hardware = "h", .reset_reason = "r"};
    gw_node_init(&node, cfg, &ops, &info);
    support_node_buffer(&node);
}

static void test_mb6_birth_and_data(void)
{
    setup_mb6();
    put_mb6(9007199254740993ull, -9007199254740993ll, 1234.56789012345, "AB", true);
    gw_node_session_up(&node, 0);
    msg(1);
    TEST_ASSERT_EQUAL_STRING("spBv1.0/G/DBIRTH/N/M", g_rec.msgs[1].topic);
    int i = decoded_find_name(&dec, "U64");
    TEST_ASSERT_EQUAL(SPB_DT_UINT64, dec.m[i].datatype);
    TEST_ASSERT_EQUAL(SPB_VAL_LONG, dec.m[i].kind);
    TEST_ASSERT_EQUAL_UINT64(9007199254740993ull, dec.m[i].u64);
    i = decoded_find_name(&dec, "I64");
    TEST_ASSERT_EQUAL(SPB_DT_INT64, dec.m[i].datatype);
    TEST_ASSERT_EQUAL_INT64(-9007199254740993ll, (int64_t)dec.m[i].u64);   /* two's complement */
    i = decoded_find_name(&dec, "F64");
    TEST_ASSERT_EQUAL(SPB_DT_DOUBLE, dec.m[i].datatype);
    TEST_ASSERT_EQUAL(SPB_VAL_DOUBLE, dec.m[i].kind);
    TEST_ASSERT_EQUAL_DOUBLE(1234.56789012345, dec.m[i].num);
    i = decoded_find_name(&dec, "Str");
    TEST_ASSERT_EQUAL(SPB_DT_STRING, dec.m[i].datatype);
    TEST_ASSERT_EQUAL_STRING("AB", dec.m[i].str);
    i = decoded_find_name(&dec, "Bit");
    TEST_ASSERT_EQUAL(SPB_DT_BOOLEAN, dec.m[i].datatype);
    TEST_ASSERT_EQUAL_DOUBLE(1, dec.m[i].num);

    /* +1 above 2^53: invisible to a double, reported exactly; deadband 5 on I64 */
    recorder_clear();
    put_mb6(9007199254740994ull, -9007199254740990ll, 1234.56789012345, "AB", true);
    gw_node_device_changed(&node, 0);
    msg(0);
    TEST_ASSERT_EQUAL(1, dec.count);                  /* I64 moved 3 < deadband 5 */
    TEST_ASSERT_EQUAL_UINT64(GW_ALIAS_TAG_BASE + 0, dec.m[0].alias);
    TEST_ASSERT_EQUAL_UINT64(9007199254740994ull, dec.m[0].u64);
    recorder_clear();
    put_mb6(9007199254740994ull, -9007199254740987ll, 1234.56789012345, "AC", false);
    gw_node_device_changed(&node, 0);
    msg(0);
    TEST_ASSERT_EQUAL(3, dec.count);                  /* I64 (6 > 5), Str, Bit */
    TEST_ASSERT_EQUAL_INT64(-9007199254740987ll, (int64_t)dec.m[0].u64);
    TEST_ASSERT_EQUAL_STRING("AC", dec.m[1].str);
    recorder_clear();
    gw_node_device_changed(&node, 0);
    TEST_ASSERT_EQUAL(0, g_rec.count);                /* nothing changed */
}

static void test_mb6_dcmd(void)
{
    setup_mb6();
    put_mb6(0, 0, 0, "", false);
    gw_node_session_up(&node, 0);
    recorder_clear();
    const char *topic = "spBv1.0/G/DCMD/N/M";
    spb_metric_t i64 = {.name = "I64", .datatype = SPB_DT_INT64, .v.i64 = 4611686018427387907ll};
    send(topic, &i64, 1);
    TEST_ASSERT_EQUAL(1, g_rec.writes);
    TEST_ASSERT_EQUAL_INT64(4611686018427387907ll, g_rec.last_write.i64);
    i64.v.i64 = INT64_MIN;
    send(topic, &i64, 1);
    TEST_ASSERT_EQUAL_INT64(INT64_MIN, g_rec.last_write.i64);
    spb_metric_t u64 = {.alias = GW_ALIAS_TAG_BASE + 0, .datatype = SPB_DT_UINT64,
                        .v.u64 = UINT64_MAX};
    send(topic, &u64, 1);
    TEST_ASSERT_EQUAL(3, g_rec.writes);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, g_rec.last_write.u64);
    /* a value in double_value with no datatype for a 64-bit tag: integral values only */
    raw_metric_t f = {.name = "U64", .no_datatype = true, .kind = RAW_DOUBLE, .num = 42.0};
    send_raw(topic, &f, 1);
    TEST_ASSERT_EQUAL(4, g_rec.writes);
    TEST_ASSERT_EQUAL_UINT64(42, g_rec.last_write.u64);
    f.num = 1.5;
    send_raw(topic, &f, 1);
    f.num = -1.0;                                     /* negative for a uint64 */
    send_raw(topic, &f, 1);
    raw_metric_t neg = {.name = "I64", .no_datatype = true, .kind = RAW_DOUBLE, .num = -7};
    send_raw(topic, &neg, 1);
    TEST_ASSERT_EQUAL(5, g_rec.writes);
    TEST_ASSERT_EQUAL_INT64(-7, g_rec.last_write.i64);
    neg.num = 1e19;                                   /* above INT64_MAX: rejected */
    send_raw(topic, &neg, 1);
    TEST_ASSERT_EQUAL(5, g_rec.writes);
    /* SPB-10 (#163): a datatype other than the one of the DBIRTH is rejected, also with an
     * integral value that fits */
    spb_metric_t other = {.name = "U64", .datatype = SPB_DT_DOUBLE, .v.num = 42.0};
    send(topic, &other, 1);
    spb_metric_t i32 = {.name = "I64", .datatype = SPB_DT_INT32, .v.num = -7};
    send(topic, &i32, 1);
    spb_metric_t big = {.name = "I64", .datatype = SPB_DT_UINT64, .v.u64 = 1};
    send(topic, &big, 1);
    TEST_ASSERT_EQUAL(5, g_rec.writes);
    /* strings: up to 2 x length characters */
    spb_metric_t str = {.name = "Str", .datatype = SPB_DT_STRING, .v.str = "OK"};
    send(topic, &str, 1);
    TEST_ASSERT_EQUAL(6, g_rec.writes);
    TEST_ASSERT_EQUAL_STRING("OK", g_rec.last_write.str);
    str.v.str = "LONG!";                             /* 5 > 2 registers x 2 */
    send(topic, &str, 1);
    spb_metric_t num = {.name = "Str", .datatype = SPB_DT_FLOAT, .v.num = 1};
    send(topic, &num, 1);
    TEST_ASSERT_EQUAL(6, g_rec.writes);
    /* register bit: a Boolean */
    spb_metric_t bit = {.name = "Bit", .datatype = SPB_DT_BOOLEAN, .v.num = 1};
    send(topic, &bit, 1);
    TEST_ASSERT_EQUAL(7, g_rec.writes);
    TEST_ASSERT_EQUAL_DOUBLE(1, g_rec.last_write.value);

    /* write confirmation of a 64-bit value: DDATA carries it exactly */
    gw_tag_value_t v = {.i64 = 4611686018427387907ll, .ts_ms = 5000, .valid = true};
    gw_tagdb_put_tag(1, &v);
    gw_node_write_done(&node, 1, true);
    msg(0);
    TEST_ASSERT_EQUAL_INT64(4611686018427387907ll, (int64_t)dec.m[0].u64);
}

/* SPB-15: writable Node Control/Scan Rate. */
static void test_scan_rate(void)
{
    gw_node_session_up(&node, 0);
    msg(0);
    int i = decoded_find_name(&dec, "Node Control/Scan Rate");
    TEST_ASSERT_TRUE(i >= 0);
    TEST_ASSERT_EQUAL(SPB_DT_UINT32, dec.m[i].datatype);
    TEST_ASSERT_EQUAL_UINT64(GW_ALIAS_SCAN_RATE, dec.m[i].alias);
    TEST_ASSERT_EQUAL_DOUBLE(0, dec.m[i].num);           /* each device at its poll_ms */
    recorder_clear();
    spb_metric_t m = {.name = "Node Control/Scan Rate", .datatype = SPB_DT_UINT32, .v.num = 3000};
    send("spBv1.0/Plant1/NCMD/GW01", &m, 1);
    TEST_ASSERT_EQUAL(1, g_rec.scan_rate_calls);
    TEST_ASSERT_EQUAL_UINT32(3000, g_rec.scan_rate);
    msg(0);                                              /* confirmed by NDATA */
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/NDATA/GW01", g_rec.msgs[0].topic);
    TEST_ASSERT_EQUAL(1, dec.count);
    TEST_ASSERT_EQUAL_UINT64(GW_ALIAS_SCAN_RATE, dec.m[0].alias);
    TEST_ASSERT_EQUAL_DOUBLE(3000, dec.m[0].num);
    /* out of range or not integral: not applied, NDATA repeats the rate in force */
    const double bad[] = {10, 3600001, 1000.5, -1};
    for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
        recorder_clear();
        spb_metric_t b = {.alias = GW_ALIAS_SCAN_RATE, .datatype = SPB_DT_DOUBLE, .v.num = bad[k]};
        send("spBv1.0/Plant1/NCMD/GW01", &b, 1);
        TEST_ASSERT_EQUAL(0, g_rec.scan_rate_calls);
        msg(0);
        TEST_ASSERT_EQUAL_DOUBLE(3000, dec.m[0].num);
    }
    /* an Int64 sender (long_value), by alias; 0 goes back to the per-device periods */
    recorder_clear();
    spb_metric_t z = {.alias = GW_ALIAS_SCAN_RATE, .datatype = SPB_DT_INT64, .v.i64 = 0};
    send("spBv1.0/Plant1/NCMD/GW01", &z, 1);
    TEST_ASSERT_EQUAL(1, g_rec.scan_rate_calls);
    TEST_ASSERT_EQUAL_UINT32(0, g_rec.scan_rate);
    /* a rebirth reports the rate in force */
    z.v.i64 = 500;
    send("spBv1.0/Plant1/NCMD/GW01", &z, 1);
    recorder_clear();
    gw_node_rebirth(&node);
    msg(0);
    TEST_ASSERT_EQUAL_DOUBLE(500, dec.m[decoded_find_name(&dec, "Node Control/Scan Rate")].num);
}

/* Issue #8: while the expander does not answer, the DOs are published as null (the latch may
 * hold outputs from before a warm reset), and NDATA carries their state once it answers. */
static void test_do_unknown_published_as_null(void)
{
    gw_tagdb_set_do_unknown(500);
    gw_node_session_up(&node, 0);
    msg(0);
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/NBIRTH/GW01", g_rec.msgs[0].topic);
    for (int k = 0; k < 8; k++) {
        int i = decoded_find_alias(&dec, GW_ALIAS_DO_BASE + (uint64_t)k);
        TEST_ASSERT_TRUE(i >= 0);
        TEST_ASSERT_TRUE(dec.m[i].is_null);
    }
    TEST_ASSERT_FALSE(dec.m[decoded_find_alias(&dec, GW_ALIAS_DI_BASE)].is_null);

    /* still unknown: nothing changed, nothing published */
    recorder_clear();
    gw_node_io_changed(&node);
    TEST_ASSERT_EQUAL(0, g_rec.count);

    /* the expander answers (all off): one NDATA with the 8 DOs and their values */
    gw_tagdb_set_do(0x00, 900);
    gw_node_io_changed(&node);
    TEST_ASSERT_EQUAL(1, g_rec.count);
    msg(0);
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/NDATA/GW01", g_rec.msgs[0].topic);
    TEST_ASSERT_EQUAL(8, dec.count);
    for (size_t i = 0; i < dec.count; i++) {
        TEST_ASSERT_FALSE(dec.m[i].is_null);
        TEST_ASSERT_EQUAL_DOUBLE(0, dec.m[i].num);
    }

    /* lost again: the 8 DOs go back to null */
    recorder_clear();
    gw_tagdb_set_do_unknown(1000);
    gw_node_io_changed(&node);
    msg(0);
    TEST_ASSERT_EQUAL(8, dec.count);
    TEST_ASSERT_TRUE(dec.m[0].is_null);
}

/* SPB-16: births carry engUnit/engLow/engHigh from the configuration and Quality; DDATA carries
 * Quality only when it changes (BAD with the null of a failed read, GOOD with the next value). */
static void test_properties_and_quality(void)
{
    static const char json[] =
        "{\"mqtt\": {\"host\": \"h\"}, \"sparkplug\": {\"group_id\": \"G\", \"edge_node_id\": \"N\"},"
        " \"devices\": [{\"name\": \"M\", \"unit_id\": 1, \"tags\": ["
        " {\"name\": \"Level\", \"area\": \"input\", \"address\": 0, \"type\": \"uint16\","
        "  \"units\": \"m\", \"eng_low\": 0, \"eng_high\": 12.5},"
        " {\"name\": \"Temp\", \"area\": \"input\", \"address\": 1, \"type\": \"int16\","
        "  \"eng_low\": -40},"
        " {\"name\": \"Run\", \"area\": \"coil\", \"address\": 0, \"type\": \"bool\"}]}]}";
    static gw_config_t c;
    gw_cfg_err_t err;
    TEST_ASSERT_TRUE_MESSAGE(gw_config_parse(json, sizeof(json) - 1, &c, &err), err.msg);
    cfg = &c;
    gw_tagdb_reset();
    gw_node_ops_t ops = recorder_ops();
    const gw_node_info_t info = {.firmware = "f", .hardware = "h", .reset_reason = "r"};
    gw_node_init(&node, cfg, &ops, &info);
    support_node_buffer(&node);
    gw_tag_value_t v[3] = {{.value = 5, .ts_ms = 1000, .valid = true},
                           {.value = 21, .ts_ms = 1000, .valid = true},
                           {.value = 0, .ts_ms = 1000, .valid = false}};   /* never read yet */
    gw_tagdb_put_device(cfg, 0, v, true);
    gw_tagdb_set_do_unknown(500);
    gw_node_session_up(&node, 0);

    msg(0);   /* NBIRTH: the DOs are unknown (expander absent): null and BAD */
    int i = decoded_find_name(&dec, "DO/1");
    TEST_ASSERT_TRUE(dec.m[i].has_quality);
    TEST_ASSERT_EQUAL_INT32(SPB_QUALITY_BAD, dec.m[i].quality);
    TEST_ASSERT_FALSE(dec.m[decoded_find_name(&dec, "DI/1")].has_quality);

    msg(1);   /* DBIRTH */
    i = decoded_find_name(&dec, "Level");
    TEST_ASSERT_EQUAL_STRING("m", dec.m[i].eng_unit);
    TEST_ASSERT_TRUE(dec.m[i].has_eng_low);
    TEST_ASSERT_EQUAL_DOUBLE(0, dec.m[i].eng_low);
    TEST_ASSERT_TRUE(dec.m[i].has_eng_high);
    TEST_ASSERT_EQUAL_DOUBLE(12.5, dec.m[i].eng_high);
    TEST_ASSERT_EQUAL_INT32(SPB_QUALITY_GOOD, dec.m[i].quality);
    TEST_ASSERT_EQUAL(4, dec.m[i].prop_count);
    i = decoded_find_name(&dec, "Temp");
    TEST_ASSERT_EQUAL_STRING("", dec.m[i].eng_unit);
    TEST_ASSERT_TRUE(dec.m[i].has_eng_low);
    TEST_ASSERT_FALSE(dec.m[i].has_eng_high);
    TEST_ASSERT_EQUAL(2, dec.m[i].prop_count);
    i = decoded_find_name(&dec, "Run");
    TEST_ASSERT_TRUE(dec.m[i].is_null);
    TEST_ASSERT_EQUAL_INT32(SPB_QUALITY_BAD, dec.m[i].quality);
    TEST_ASSERT_EQUAL(1, dec.m[i].prop_count);

    /* Level changes (still good): no Quality; Run read for the first time: GOOD */
    recorder_clear();
    v[0].value = 6;
    v[2].valid = true;
    v[2].value = 1;
    gw_tagdb_put_device(cfg, 0, v, true);
    gw_node_device_changed(&node, 0);
    msg(0);
    TEST_ASSERT_EQUAL(2, dec.count);
    i = decoded_find_alias(&dec, GW_ALIAS_TAG_BASE + 0);
    TEST_ASSERT_EQUAL(0, dec.m[i].prop_count);
    i = decoded_find_alias(&dec, GW_ALIAS_TAG_BASE + 2);
    TEST_ASSERT_TRUE(dec.m[i].has_quality);
    TEST_ASSERT_EQUAL_INT32(SPB_QUALITY_GOOD, dec.m[i].quality);
    TEST_ASSERT_EQUAL_STRING("", dec.m[i].eng_unit);   /* engUnit only in the birth */

    /* a failed read (exception): null with BAD, once */
    recorder_clear();
    v[1].valid = false;
    gw_tagdb_put_device(cfg, 0, v, true);
    gw_node_device_changed(&node, 0);
    gw_node_device_changed(&node, 0);
    TEST_ASSERT_EQUAL(1, g_rec.count);
    msg(0);
    TEST_ASSERT_EQUAL(1, dec.count);
    TEST_ASSERT_TRUE(dec.m[0].is_null);
    TEST_ASSERT_EQUAL_INT32(SPB_QUALITY_BAD, dec.m[0].quality);

    /* the expander answers: the 8 DOs with GOOD */
    recorder_clear();
    gw_tagdb_set_do(0x00, 900);
    gw_node_io_changed(&node);
    msg(0);
    TEST_ASSERT_EQUAL(8, dec.count);
    TEST_ASSERT_EQUAL_INT32(SPB_QUALITY_GOOD, dec.m[0].quality);
    /* a DO change afterwards: no Quality */
    recorder_clear();
    gw_tagdb_set_do(0x01, 950);
    gw_node_io_changed(&node);
    msg(0);
    TEST_ASSERT_EQUAL(1, dec.count);
    TEST_ASSERT_FALSE(dec.m[0].has_quality);
}

/* SPB-19: a bytes tag is a Bytes metric (raw register bytes); report by exception over the
 * bytes; DCMD with a Bytes value of exactly 2 x length bytes. */
static void test_bytes_tag(void)
{
    static const char json[] =
        "{\"mqtt\": {\"host\": \"h\"}, \"sparkplug\": {\"group_id\": \"G\", \"edge_node_id\": \"N\"},"
        " \"devices\": [{\"name\": \"M\", \"unit_id\": 1, \"tags\": ["
        " {\"name\": \"Raw\", \"area\": \"holding\", \"address\": 0, \"type\": \"bytes\","
        "  \"length\": 2, \"writable\": true}]}]}";
    static gw_config_t c;
    gw_cfg_err_t err;
    TEST_ASSERT_TRUE_MESSAGE(gw_config_parse(json, sizeof(json) - 1, &c, &err), err.msg);
    cfg = &c;
    gw_tagdb_reset();
    gw_node_ops_t ops = recorder_ops();
    const gw_node_info_t info = {.firmware = "f", .hardware = "h", .reset_reason = "r"};
    gw_node_init(&node, cfg, &ops, &info);
    support_node_buffer(&node);
    gw_tag_value_t v = {.ts_ms = 1000, .valid = true};
    memcpy(v.str, "\x00\x01\x00\x02", 4);   /* NUL first: not a C string */
    gw_tagdb_put_device(cfg, 0, &v, true);
    gw_node_session_up(&node, 0);
    msg(1);
    TEST_ASSERT_EQUAL(SPB_DT_BYTES, dec.m[0].datatype);
    TEST_ASSERT_EQUAL(4, dec.m[0].bytes_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY("\x00\x01\x00\x02", dec.m[0].bytes, 4);

    recorder_clear();
    gw_node_device_changed(&node, 0);
    TEST_ASSERT_EQUAL(0, g_rec.count);                /* unchanged */
    v.str[3] = 0x03;                                  /* a change after the NUL is seen */
    gw_tagdb_put_device(cfg, 0, &v, true);
    gw_node_device_changed(&node, 0);
    msg(0);
    TEST_ASSERT_EQUAL(1, dec.count);
    TEST_ASSERT_EQUAL_HEX8(0x03, dec.m[0].bytes[3]);

    recorder_clear();
    static const uint8_t w[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    spb_metric_t b = {.name = "Raw", .datatype = SPB_DT_BYTES, .v.bytes = {w, 4}};
    send("spBv1.0/G/DCMD/N/M", &b, 1);
    TEST_ASSERT_EQUAL(1, g_rec.writes);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(w, g_rec.last_write.str, 4);
    b.v.bytes.len = 3;                                /* not the tag's 4 bytes */
    send("spBv1.0/G/DCMD/N/M", &b, 1);
    spb_metric_t s = {.name = "Raw", .datatype = SPB_DT_STRING, .v.str = "ABCD"};
    send("spBv1.0/G/DCMD/N/M", &s, 1);                /* a String is not Bytes */
    TEST_ASSERT_EQUAL(1, g_rec.writes);
}

/* SPB-18: devices of a type are template instances; the NBIRTH defines each type once. */
static void test_templates(void)
{
    static const char json[] =
        "{\"mqtt\": {\"host\": \"h\"}, \"sparkplug\": {\"group_id\": \"G\", \"edge_node_id\": \"N\"},"
        " \"devices\": ["
        " {\"name\": \"P1\", \"unit_id\": 1, \"type\": \"Pump\", \"tags\": ["
        "  {\"name\": \"Speed\", \"area\": \"holding\", \"address\": 0, \"type\": \"uint16\","
        "   \"units\": \"rpm\", \"writable\": true},"
        "  {\"name\": \"Run\", \"area\": \"coil\", \"address\": 0, \"type\": \"bool\"}]},"
        " {\"name\": \"P2\", \"unit_id\": 2, \"type\": \"Pump\", \"tags\": ["
        "  {\"name\": \"Speed\", \"area\": \"holding\", \"address\": 0, \"type\": \"uint16\","
        "   \"writable\": true},"
        "  {\"name\": \"Run\", \"area\": \"coil\", \"address\": 0, \"type\": \"bool\"}]},"
        " {\"name\": \"Flat\", \"unit_id\": 3, \"tags\": ["
        "  {\"name\": \"X\", \"area\": \"holding\", \"address\": 0, \"type\": \"uint16\"}]}]}";
    static gw_config_t c;
    gw_cfg_err_t err;
    TEST_ASSERT_TRUE_MESSAGE(gw_config_parse(json, sizeof(json) - 1, &c, &err), err.msg);
    cfg = &c;
    gw_tagdb_reset();
    gw_node_ops_t ops = recorder_ops();
    const gw_node_info_t info = {.firmware = "f", .hardware = "h", .reset_reason = "r"};
    gw_node_init(&node, cfg, &ops, &info);
    support_node_buffer(&node);
    gw_tag_value_t v[2] = {{.value = 1500, .ts_ms = 1000, .valid = true},
                           {.value = 1, .ts_ms = 1000, .valid = true}};
    gw_tagdb_put_device(cfg, 0, v, true);
    gw_tagdb_put_device(cfg, 2, v, true);
    gw_node_session_up(&node, 0);
    TEST_ASSERT_EQUAL(3, g_rec.count);             /* NBIRTH, DBIRTH P1, DBIRTH Flat */

    msg(0);   /* NBIRTH: one definition for the two pumps, after the node's metrics */
    TEST_ASSERT_EQUAL(GW_NODE_BIRTH_METRICS - 1 + 1, dec.count);
    int i = decoded_find_name(&dec, "Pump");
    TEST_ASSERT_TRUE(i >= 0);
    TEST_ASSERT_EQUAL(SPB_DT_TEMPLATE, dec.m[i].datatype);
    TEST_ASSERT_FALSE(dec.m[i].has_alias);
    TEST_ASSERT_TRUE(dec.m[i].is_definition);
    TEST_ASSERT_FALSE(dec.m[i].has_ref);
    TEST_ASSERT_EQUAL(2, dec.mem_count);
    TEST_ASSERT_EQUAL_STRING("Speed", dec.mem[0].name);
    TEST_ASSERT_EQUAL(SPB_DT_UINT16, dec.mem[0].datatype);
    TEST_ASSERT_FALSE(dec.mem[0].has_alias);
    TEST_ASSERT_EQUAL_STRING("rpm", dec.mem[0].eng_unit);   /* from the first device of the type */
    TEST_ASSERT_FALSE(dec.mem[0].has_quality);
    TEST_ASSERT_EQUAL_STRING("Run", dec.mem[1].name);

    msg(1);   /* DBIRTH P1: one instance metric */
    TEST_ASSERT_EQUAL_STRING("spBv1.0/G/DBIRTH/N/P1", g_rec.msgs[1].topic);
    TEST_ASSERT_EQUAL(1, dec.count);
    TEST_ASSERT_EQUAL_STRING("Pump", dec.m[0].name);
    TEST_ASSERT_EQUAL_UINT64(GW_ALIAS_TAG_BASE + 0, dec.m[0].alias);
    TEST_ASSERT_EQUAL(SPB_DT_TEMPLATE, dec.m[0].datatype);
    TEST_ASSERT_FALSE(dec.m[0].is_definition);
    TEST_ASSERT_EQUAL_STRING("Pump", dec.m[0].ref);
    TEST_ASSERT_EQUAL(2, dec.mem_count);
    TEST_ASSERT_EQUAL_DOUBLE(1500, dec.mem[0].num);
    TEST_ASSERT_EQUAL_INT32(SPB_QUALITY_GOOD, dec.mem[0].quality);
    TEST_ASSERT_EQUAL_STRING("rpm", dec.mem[0].eng_unit);
    msg(2);   /* the untyped device as before */
    TEST_ASSERT_EQUAL_STRING("X", dec.m[0].name);
    TEST_ASSERT_EQUAL_UINT64(GW_ALIAS_TAG_BASE + 4, dec.m[0].alias);

    /* DDATA: the instance by alias, with the changed member only (by name) */
    recorder_clear();
    v[0].value = 1600;
    gw_tagdb_put_device(cfg, 0, v, true);
    gw_node_device_changed(&node, 0);
    msg(0);
    TEST_ASSERT_EQUAL(1, dec.count);
    TEST_ASSERT_FALSE(dec.m[0].has_name);
    TEST_ASSERT_EQUAL_UINT64(GW_ALIAS_TAG_BASE + 0, dec.m[0].alias);
    TEST_ASSERT_EQUAL(1, dec.mem_count);
    TEST_ASSERT_EQUAL_STRING("Speed", dec.mem[0].name);
    TEST_ASSERT_EQUAL_DOUBLE(1600, dec.mem[0].num);
    TEST_ASSERT_FALSE(dec.mem[0].has_quality);
    /* tck-id-payloads-metric-datatype-not-req: no datatype on the instance or its members in
     * DATA; the DBIRTH and the NBIRTH definition have them (#159) */
    TEST_ASSERT_FALSE(dec.m[0].has_datatype);
    TEST_ASSERT_FALSE(dec.mem[0].has_datatype);

    /* DCMD: a member of the instance, or the tag by name; never by alias */
    recorder_clear();
    const spb_metric_t member = {.name = "Speed", .no_alias = true, .datatype = SPB_DT_UINT16,
                                 .v.num = 900};
    const spb_template_t inst = {.ref = "Pump", .count = 1, .members = &member};
    const spb_metric_t cmd_m = {.name = "Pump", .alias = GW_ALIAS_TAG_BASE, .datatype = SPB_DT_TEMPLATE,
                                .v.tmpl = &inst};
    send("spBv1.0/G/DCMD/N/P1", &cmd_m, 1);
    TEST_ASSERT_EQUAL(1, g_rec.writes);
    TEST_ASSERT_EQUAL_UINT16(0, g_rec.last_write_tag);
    TEST_ASSERT_EQUAL_DOUBLE(900, g_rec.last_write.value);
    spb_metric_t flat = {.name = "Speed", .datatype = SPB_DT_UINT16, .v.num = 901};
    send("spBv1.0/G/DCMD/N/P1", &flat, 1);
    TEST_ASSERT_EQUAL(2, g_rec.writes);
    flat.name = NULL;
    flat.alias = GW_ALIAS_TAG_BASE + 0;
    send("spBv1.0/G/DCMD/N/P1", &flat, 1);
    TEST_ASSERT_EQUAL(2, g_rec.writes);

    /* the write confirmation: the instance with that member */
    gw_tag_value_t w = {.value = 900, .ts_ms = 2000, .valid = true};
    gw_tagdb_put_tag(0, &w);
    recorder_clear();
    gw_node_write_done(&node, 0, true);
    msg(0);
    TEST_ASSERT_EQUAL(SPB_VAL_TEMPLATE, dec.m[0].kind);
    TEST_ASSERT_FALSE(dec.m[0].has_datatype);   /* DDATA: no datatype (#159) */
    TEST_ASSERT_EQUAL(1, dec.mem_count);
    TEST_ASSERT_EQUAL_DOUBLE(900, dec.mem[0].num);
    TEST_ASSERT_FALSE(dec.mem[0].has_datatype);
}

/* SPB-18 + SPB-16: the largest NBIRTH the configuration allows (GW_MAX_TEMPLATE_TAGS template
 * tags with 39-character names, units and an engineering range) fits the payload buffer. */
static void test_worst_case_nbirth_fits(void)
{
    static char doc[32768];
    static gw_config_t c;
    size_t n = (size_t)snprintf(doc, sizeof(doc), "{\"mqtt\": {\"host\": \"h\"}, \"sparkplug\": "
                                "{\"group_id\": \"G\", \"edge_node_id\": \"N\"}, \"devices\": [");
    const int types = 16, per = GW_MAX_TEMPLATE_TAGS / 16;
    for (int d = 0; d < types; d++) {
        n += (size_t)snprintf(doc + n, sizeof(doc) - n, "%s{\"name\": \"%039d\", \"unit_id\": 1, "
                              "\"type\": \"%039d\", \"tags\": [", d ? "," : "", d, d);
        for (int k = 0; k < per; k++) {
            n += (size_t)snprintf(doc + n, sizeof(doc) - n,
                                  "%s{\"name\": \"%038d%d\", \"area\": \"input\", \"address\": %d,"
                                  " \"type\": \"float64\", \"units\": \"uuuuuuuuuuuuuuu\","
                                  " \"eng_low\": -3e38, \"eng_high\": 3e38}",
                                  k ? "," : "", 0, k, 4 * k);
        }
        n += (size_t)snprintf(doc + n, sizeof(doc) - n, "]}");
    }
    snprintf(doc + n, sizeof(doc) - n, "]}");
    gw_cfg_err_t err;
    TEST_ASSERT_TRUE_MESSAGE(gw_config_parse(doc, strlen(doc), &c, &err), err.msg);
    cfg = &c;
    c.modbus_tcp.enabled = true;   /* #81: the Modbus TCP table too */
    gw_tagdb_reset();
    gw_node_ops_t ops = recorder_ops();
    const gw_node_info_t info = {.firmware = "0.1.0+g0123456.dirty-xxxxxxxxxxxxxxxxxxxxxxxxx",
                                 .hardware = "Waveshare ESP32-S3-POE-ETH-8DI-8DO (ESP32-S3 rev 0.2)",
                                 .reset_reason = "interrupt watchdog"};
    gw_node_init(&node, cfg, &ops, &info);
    support_node_buffer(&node);
    /* SPB-17: the widest diagnostics (RSSI, every counter of every device at its maximum) */
    g_rec.diag = (gw_node_diag_t){.uptime_s = UINT64_MAX, .free_heap = UINT32_MAX,
                                  .min_free_heap = UINT32_MAX, .has_rssi = true,
                                  .rssi_valid = true, .rssi = -128};
    const gw_dev_stats_t full = {UINT32_MAX, UINT32_MAX,
                                 {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX},
                                 {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX}};
    for (uint16_t d = 0; d < 16; d++) {
        gw_tagdb_put_stats(d, &full);
    }
    const gw_mbap_stats_t gfull = {.other_units = full.tcp};
    gw_mbap_stats_reset();
    gw_mbap_stats_add(&gfull);
    gw_node_session_up(&node, 0);
    TEST_ASSERT_EQUAL(1, g_rec.count);   /* the NBIRTH went out (devices offline) */
    printf("worst-case NBIRTH with %d template tags: %u bytes of %u\n", GW_MAX_TEMPLATE_TAGS,
           (unsigned)g_rec.msgs[0].len, (unsigned)GW_SPB_PAYLOAD_MAX);
    TEST_ASSERT_LESS_THAN(GW_SPB_PAYLOAD_MAX, g_rec.msgs[0].len);
    msg(0);
    TEST_ASSERT_EQUAL(GW_NODE_BIRTH_METRICS + types, dec.count);
    /* without PSRAM the buffer is smaller (CONFIG_GW_SPB_PAYLOAD_INTERNAL): such a birth is
     * not published (and logged), never cut */
    static uint8_t small[8192];
    gw_node_set_buffer(&node, small, sizeof(small));
    recorder_clear();
    TEST_ASSERT_FALSE(gw_node_rebirth(&node));
    TEST_ASSERT_EQUAL(0, g_rec.count);
}

/* SPB-17 / MB-13: diagnostics in the NBIRTH; every diagnostics_s an NDATA with uptime and heap,
 * and the Modbus DataSet only when a counter changed. */
static int mb_rows, mb_cells;
static uint32_t mb_ok_row0;
static char mb_dev0[16];
static bool mb_cell_cb(void *ctx, uint32_t row, uint32_t col, const spb_rx_value_t *v)
{
    (void)ctx;
    mb_cells++;
    if (row + 1 > (uint32_t)mb_rows) {
        mb_rows = (int)row + 1;
    }
    if (row == 0 && col == 0) {
        snprintf(mb_dev0, sizeof(mb_dev0), "%s", v->v.s);
    }
    if (row == 0 && col == 4) {
        mb_ok_row0 = v->v.i;
    }
    return true;
}

static void test_diagnostics(void)
{
    set_meter(true, 230.5, 12.3, 1);
    const gw_dev_stats_t st = {.cycles = 7, .failed_cycles = 1, .bus = {.ok = 20, .timeout = 3}};
    gw_tagdb_put_stats(0, &st);
    g_rec.diag.rssi_valid = false;
    gw_node_session_up(&node, 0);
    msg(0);
    int i = decoded_find_name(&dec, "Node Info/Uptime");
    TEST_ASSERT_EQUAL(SPB_DT_UINT64, dec.m[i].datatype);
    TEST_ASSERT_EQUAL_UINT64(GW_ALIAS_UPTIME, dec.m[i].alias);
    TEST_ASSERT_EQUAL_UINT64(12, dec.m[i].u64);
    i = decoded_find_name(&dec, "Node Info/Free Heap");
    TEST_ASSERT_EQUAL(SPB_DT_UINT32, dec.m[i].datatype);
    TEST_ASSERT_EQUAL_DOUBLE(150000, dec.m[i].num);
    TEST_ASSERT_TRUE(decoded_find_name(&dec, "Node Info/Min Free Heap") >= 0);
    i = decoded_find_name(&dec, "Node Info/RSSI");
    TEST_ASSERT_EQUAL(SPB_DT_INT32, dec.m[i].datatype);
    TEST_ASSERT_TRUE(dec.m[i].is_null);                      /* Wi-Fi not associated */
    i = decoded_find_name(&dec, "Node Info/Modbus");
    TEST_ASSERT_EQUAL(SPB_DT_DATASET, dec.m[i].datatype);
    TEST_ASSERT_EQUAL_UINT64(GW_ALIAS_MODBUS, dec.m[i].alias);
    mb_rows = mb_cells = 0;
    const spb_decoder_t d = {.cell = mb_cell_cb};
    TEST_ASSERT_TRUE(spb_decode_with(g_rec.msgs[0].payload, g_rec.msgs[0].len, NULL, &d));
    TEST_ASSERT_EQUAL(1, mb_rows);
    TEST_ASSERT_EQUAL(9, mb_cells);
    TEST_ASSERT_EQUAL_STRING("Meter1", mb_dev0);
    TEST_ASSERT_EQUAL_UINT32(20, mb_ok_row0);

    /* rate limited: nothing before diagnostics_s (60 s by default) */
    recorder_clear();
    TEST_ASSERT_EQUAL_UINT32(60000, gw_node_diag_tick(&node, 1000));   /* the period starts */
    TEST_ASSERT_EQUAL_UINT32(30000, gw_node_diag_tick(&node, 31000));
    TEST_ASSERT_EQUAL(0, g_rec.count);
    g_rec.diag.uptime_s = 72;
    g_rec.diag.rssi_valid = true;
    g_rec.diag.rssi = -61;
    gw_node_diag_tick(&node, 61000);                         /* counters unchanged */
    msg(0);
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/NDATA/GW01", g_rec.msgs[0].topic);
    TEST_ASSERT_EQUAL(4, dec.count);                         /* uptime, 2 x heap, RSSI */
    TEST_ASSERT_EQUAL_UINT64(72, dec.m[decoded_find_alias(&dec, GW_ALIAS_UPTIME)].u64);
    i = decoded_find_alias(&dec, GW_ALIAS_RSSI);
    TEST_ASSERT_FALSE(dec.m[i].is_null);
    /* no datatype in NDATA (#159): the host reads int_value with the Int32 of the NBIRTH */
    TEST_ASSERT_FALSE(dec.m[i].has_datatype);
    TEST_ASSERT_EQUAL_INT32(-61, (int32_t)(uint32_t)dec.m[i].num);
    TEST_ASSERT_EQUAL(-1, decoded_find_alias(&dec, GW_ALIAS_MODBUS));
    recorder_clear();
    const gw_dev_stats_t st2 = {.cycles = 70, .failed_cycles = 1, .bus = {.ok = 200, .timeout = 3}};
    gw_tagdb_put_stats(0, &st2);
    gw_node_diag_tick(&node, 121000);
    msg(0);
    TEST_ASSERT_EQUAL(5, dec.count);
    TEST_ASSERT_TRUE(decoded_find_alias(&dec, GW_ALIAS_MODBUS) >= 0);
    mb_rows = mb_cells = 0;
    TEST_ASSERT_TRUE(spb_decode_with(g_rec.msgs[0].payload, g_rec.msgs[0].len, NULL, &d));
    TEST_ASSERT_EQUAL_UINT32(200, mb_ok_row0);

    /* session down: nothing; diagnostics_s 0 = births only */
    gw_node_session_down(&node);
    recorder_clear();
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, gw_node_diag_tick(&node, 500000));
    TEST_ASSERT_EQUAL(0, g_rec.count);
    static gw_config_t c;
    c = *cfg;
    c.sparkplug.diagnostics_s = 0;
    gw_node_ops_t ops = recorder_ops();
    const gw_node_info_t info = {.firmware = "f", .hardware = "h", .reset_reason = "r"};
    gw_node_init(&node, &c, &ops, &info);
    support_node_buffer(&node);
    gw_node_session_up(&node, 0);
    recorder_clear();
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, gw_node_diag_tick(&node, 1));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, gw_node_diag_tick(&node, 10000000));
    TEST_ASSERT_EQUAL(0, g_rec.count);
}

/* Issue #81: with the Modbus TCP gateway enabled, "Node Info/Modbus TCP" has a row per device
 * with the forwarded requests to its unit, and one for units without a device. */
static uint32_t tcp_rows, tcp_cells, tcp_unit0, tcp_timeout_other, tcp_ok0;
static bool tcp_online0;
static char tcp_dev[2][16];
static bool tcp_cell_cb(void *ctx, uint32_t row, uint32_t col, const spb_rx_value_t *v)
{
    (void)ctx;
    tcp_cells++;
    tcp_rows = row + 1 > tcp_rows ? row + 1 : tcp_rows;
    if (col == 0 && row < 2) {
        snprintf(tcp_dev[row], sizeof(tcp_dev[row]), "%s", v->v.s);
    }
    if (row == 0 && col == 1) tcp_unit0 = v->v.i;
    if (row == 0 && col == 2) tcp_online0 = v->v.b;
    if (row == 0 && col == 3) tcp_ok0 = v->v.i;
    if (row == 1 && col == 4) tcp_timeout_other = v->v.i;
    return true;
}

static void test_modbus_tcp_table(void)
{
    static gw_config_t c;
    c = *cfg;
    c.modbus_tcp.enabled = true;
    cfg = &c;
    gw_tagdb_reset();
    gw_mbap_stats_reset();
    gw_node_ops_t ops = recorder_ops();
    const gw_node_info_t info = {.firmware = "f", .hardware = "h", .reset_reason = "r"};
    gw_node_init(&node, cfg, &ops, &info);
    support_node_buffer(&node);
    set_meter(true, 230.5, 12.3, 1);
    const gw_dev_stats_t st = {.cycles = 5, .bus = {.ok = 9}, .tcp = {.ok = 3, .exception = 1}};
    gw_tagdb_put_stats(0, &st);
    const gw_mbap_stats_t g = {.other_units = {.timeout = 2}};
    gw_mbap_stats_add(&g);
    gw_node_session_up(&node, 0);
    msg(0);
    TEST_ASSERT_EQUAL(GW_NODE_BIRTH_METRICS, dec.count);
    int i = decoded_find_name(&dec, "Node Info/Modbus TCP");
    TEST_ASSERT_TRUE(i >= 0);
    TEST_ASSERT_EQUAL(SPB_DT_DATASET, dec.m[i].datatype);
    TEST_ASSERT_EQUAL_UINT64(GW_ALIAS_MODBUS_TCP, dec.m[i].alias);
    /* the periodic NDATA after only a TCP counter moved carries both tables */
    recorder_clear();
    gw_node_diag_tick(&node, 1000);
    const gw_mbap_stats_t g2 = {.other_units = {.timeout = 1}};
    gw_mbap_stats_add(&g2);                                    /* only a TCP counter moves */
    gw_node_diag_tick(&node, 61000);
    msg(0);
    TEST_ASSERT_TRUE(decoded_find_alias(&dec, GW_ALIAS_MODBUS_TCP) >= 0);
    TEST_ASSERT_TRUE(decoded_find_alias(&dec, GW_ALIAS_MODBUS) >= 0);   /* published together */
    /* the cells of both tables reach the callback, the TCP table's last (alias order) */
    tcp_rows = tcp_cells = 0;
    const spb_decoder_t d = {.cell = tcp_cell_cb};
    TEST_ASSERT_TRUE(spb_decode_with(g_rec.msgs[0].payload, g_rec.msgs[0].len, NULL, &d));
    TEST_ASSERT_EQUAL(9 + 2 * 8, tcp_cells);   /* RTU table (1 row x 9) + TCP table (2 x 8) */
    TEST_ASSERT_EQUAL_STRING("(other units)", tcp_dev[1]);
    TEST_ASSERT_EQUAL_UINT32(3, tcp_timeout_other);           /* 2 + 1 */
    TEST_ASSERT_EQUAL_UINT32(1, tcp_unit0);                   /* Meter1 is unit 1 */
    TEST_ASSERT_TRUE(tcp_online0);
    TEST_ASSERT_EQUAL_UINT32(3, tcp_ok0);
    gw_mbap_stats_reset();
}

/* TIME-3: Node Info/Time Synced in the NBIRTH, and an NDATA each time it changes. */
static void test_time_synced(void)
{
    g_rec.diag.time_synced = false;   /* e.g. the clock came from the RTC */
    gw_node_session_up(&node, 0);
    msg(0);
    int i = decoded_find_name(&dec, "Node Info/Time Synced");
    TEST_ASSERT_TRUE(i >= 0);
    TEST_ASSERT_EQUAL(SPB_DT_BOOLEAN, dec.m[i].datatype);
    TEST_ASSERT_EQUAL_UINT64(GW_ALIAS_TIME_SYNCED, dec.m[i].alias);
    TEST_ASSERT_EQUAL_DOUBLE(0, dec.m[i].num);
    recorder_clear();
    gw_node_set_time_synced(&node, false);           /* unchanged */
    TEST_ASSERT_EQUAL(0, g_rec.count);
    gw_node_set_time_synced(&node, true);            /* SNTP answered */
    msg(0);
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/NDATA/GW01", g_rec.msgs[0].topic);
    TEST_ASSERT_EQUAL(1, dec.count);
    TEST_ASSERT_EQUAL_UINT64(GW_ALIAS_TIME_SYNCED, dec.m[0].alias);
    TEST_ASSERT_EQUAL_DOUBLE(1, dec.m[0].num);
    gw_node_set_time_synced(&node, true);
    TEST_ASSERT_EQUAL(1, g_rec.count);
    /* a rebirth takes the state from the diagnostics op, without a second NDATA */
    g_rec.diag.time_synced = true;
    recorder_clear();
    gw_node_rebirth(&node);
    msg(0);
    TEST_ASSERT_EQUAL_DOUBLE(1, dec.m[decoded_find_name(&dec, "Node Info/Time Synced")].num);
    size_t births = g_rec.count;
    gw_node_set_time_synced(&node, true);
    TEST_ASSERT_EQUAL(births, g_rec.count);
    gw_node_session_down(&node);
    gw_node_set_time_synced(&node, false);           /* offline: nothing */
    TEST_ASSERT_EQUAL(births, g_rec.count);
}

/* Issue #14: a command metric without an alias is not logged as "alias 0". */
static void test_discard_log_shows_alias_only_if_present(void)
{
    static char log[2048];
    set_meter(true, 1, 2, 0);
    gw_node_session_up(&node, 0);
    gw_port_log_capture(log, sizeof(log));
    raw_metric_t by_name = {.name = "Voltage", .datatype = SPB_DT_FLOAT, .kind = RAW_FLOAT, .num = 1};
    send_raw("spBv1.0/Plant1/DCMD/GW01/Meter1", &by_name, 1);
    TEST_ASSERT_NOT_NULL(strstr(log, "metric 'Voltage' discarded: read-only metric"));
    TEST_ASSERT_NULL(strstr(log, "alias"));
    log[0] = '\0';
    raw_metric_t by_alias = {.has_alias = true, .alias = GW_ALIAS_TAG_BASE + VOLTAGE,
                             .datatype = SPB_DT_FLOAT, .kind = RAW_FLOAT, .num = 1};
    send_raw("spBv1.0/Plant1/DCMD/GW01/Meter1", &by_alias, 1);
    TEST_ASSERT_NOT_NULL(strstr(log, "metric '' (alias 100) discarded: read-only metric"));
    log[0] = '\0';
    raw_metric_t alias0 = {.name = "Nope", .has_alias = true, .alias = 0,
                           .datatype = SPB_DT_BOOLEAN, .kind = RAW_BOOL, .num = 1};
    send_raw("spBv1.0/Plant1/NCMD/GW01", &alias0, 1);   /* an explicit alias 0 is shown */
    TEST_ASSERT_NOT_NULL(strstr(log, "metric 'Nope' (alias 0) discarded"));
    gw_port_log_capture(NULL, 0);
    TEST_ASSERT_EQUAL(0, g_rec.writes);
}

/* Issue #10: nanopb aborts on a name longer than 63 or a string longer than 256 characters; the metrics
 * decoded before it must not be applied either. */
static void test_command_aborted_mid_payload_applies_nothing(void)
{
    static const char long_name[] =
        "A_metric_name_that_is_much_longer_than_the_sixty_three_characters_accepted";
    /* issue #87: strings now go up to 256 characters: this one is longer */
    static char long_str[SPB_STRING_MAX + 40];
    memset(long_str, 'x', sizeof(long_str) - 1);
    long_str[sizeof(long_str) - 1] = '\0';
    set_meter(true, 1, 2, 0);
    gw_node_session_up(&node, 0);
    recorder_clear();

    raw_metric_t ncmd[3] = {
        {.name = "DO/1", .datatype = SPB_DT_BOOLEAN, .kind = RAW_BOOL, .num = 1},
        {.name = long_name, .datatype = SPB_DT_BOOLEAN, .kind = RAW_BOOL, .num = 1},
        {.name = "DO/2", .datatype = SPB_DT_BOOLEAN, .kind = RAW_BOOL, .num = 1},
    };
    send_raw("spBv1.0/Plant1/NCMD/GW01", ncmd, 3);
    TEST_ASSERT_EQUAL_HEX8(0, g_rec.dout);   /* DO/1 untouched */
    TEST_ASSERT_EQUAL(0, g_rec.count);       /* and no NDATA */

    ncmd[1] = (raw_metric_t){.name = "Note", .datatype = SPB_DT_STRING, .kind = RAW_STRING,
                             .str = long_str};
    send_raw("spBv1.0/Plant1/NCMD/GW01", ncmd, 3);
    TEST_ASSERT_EQUAL_HEX8(0, g_rec.dout);

    raw_metric_t dcmd[2] = {
        {.name = "Setpoint", .datatype = SPB_DT_FLOAT, .kind = RAW_FLOAT, .num = 12.3},
        {.name = long_name, .datatype = SPB_DT_FLOAT, .kind = RAW_FLOAT, .num = 1},
    };
    send_raw("spBv1.0/Plant1/DCMD/GW01/Meter1", dcmd, 2);
    TEST_ASSERT_EQUAL(0, g_rec.writes);

    /* the same commands without the offending metric are applied in full */
    ncmd[1] = (raw_metric_t){.name = "Nope", .datatype = SPB_DT_BOOLEAN, .kind = RAW_BOOL, .num = 1};
    send_raw("spBv1.0/Plant1/NCMD/GW01", ncmd, 3);
    TEST_ASSERT_EQUAL_HEX8(0x03, g_rec.dout);
    TEST_ASSERT_EQUAL(1, g_rec.count);       /* one NDATA confirming DO/1 and DO/2 */
    send_raw("spBv1.0/Plant1/DCMD/GW01/Meter1", dcmd, 1);
    TEST_ASSERT_EQUAL(1, g_rec.writes);
    TEST_ASSERT_DOUBLE_WITHIN(1e-5, 12.3, g_rec.last_write_value);
}

static void test_session_down_suppresses_publishing(void)
{
    set_meter(true, 1, 2, 0);
    gw_node_session_up(&node, 0);
    gw_node_session_down(&node);
    recorder_clear();
    set_meter(true, 5, 2, 0);
    gw_node_device_changed(&node, 0);
    gw_node_io_changed(&node);
    spb_metric_t m = {.name = "Node Control/Rebirth", .datatype = SPB_DT_BOOLEAN, .v.num = 1};
    send("spBv1.0/Plant1/NCMD/GW01", &m, 1);
    TEST_ASSERT_EQUAL(0, g_rec.count);
    gw_node_session_up(&node, 1);  /* new session: births carry current values */
    TEST_ASSERT_EQUAL(2, g_rec.count);
    msg(1);
    TEST_ASSERT_EQUAL_DOUBLE(5, dec.m[decoded_find_name(&dec, "Voltage")].num);
}

/* SPB-14: without a store (the MVP) changes while offline are coalesced: nothing is kept. */
static void test_no_store_coalesces(void)
{
    set_meter(true, 230.5, 12.3, 1);
    gw_node_session_up(&node, 1);
    gw_node_session_down(&node);
    set_meter(true, 231.0, 12.3, 1);
    gw_node_device_changed(&node, 0);
    TEST_ASSERT_EQUAL_size_t(0, gw_node_flush_history(&node, 10));
    recorder_clear();
    gw_node_session_up(&node, 2);
    TEST_ASSERT_EQUAL(2, g_rec.count);   /* births only */
}

/* SPB-14: changes while offline are stored with the same exception and deadband rules, and
 * after the births they come out, oldest first, as DDATA/NDATA with is_historical and their
 * sample times; seq continues; a device without DBIRTH in the new session is dropped. */
static void test_store_and_forward(void)
{
    static gw_sf_rec_t storage[64];
    gw_sf_t sf;
    gw_sf_init(&sf, storage, 64);
    gw_node_set_store(&node, &sf);
    set_meter(true, 230.5, 12.3, 1);
    gw_node_session_up(&node, 1);
    gw_node_session_down(&node);

    /* offline: three polls; the second one is inside the 0.1 deadband of Voltage */
    gw_tag_value_t v[3] = {{.value = 231.0, .ts_ms = 2000, .valid = true},
                           {.value = 12.3, .ts_ms = 2000, .valid = true},
                           {.value = 1, .ts_ms = 2000, .valid = true}};
    gw_tagdb_put_device(cfg, 0, v, true);
    gw_node_device_changed(&node, 0);
    v[0].value = 231.05;
    v[0].ts_ms = 3000;
    gw_tagdb_put_device(cfg, 0, v, true);
    gw_node_device_changed(&node, 0);
    v[0].value = 232.0;
    v[0].ts_ms = 4000;
    v[2].value = 0;
    v[2].ts_ms = 4000;
    gw_tagdb_put_device(cfg, 0, v, true);
    gw_node_device_changed(&node, 0);
    gw_tagdb_set_di(0x01, 4500);
    gw_node_io_changed(&node);
    gw_tagdb_set_di(0x00, 4600);
    gw_node_io_changed(&node);
    TEST_ASSERT_EQUAL_size_t(5, gw_sf_count(&sf));   /* 231, (232, Run), DI/1 on, DI/1 off */
    TEST_ASSERT_EQUAL(0, g_rec.count - 2);           /* nothing published while offline */
    /* flushing while offline publishes nothing */
    TEST_ASSERT_EQUAL_size_t(5, gw_node_flush_history(&node, 10));

    recorder_clear();
    gw_node_session_up(&node, 2);
    TEST_ASSERT_EQUAL(2, g_rec.count);               /* NBIRTH, DBIRTH with the current values */
    msg(1);
    TEST_ASSERT_EQUAL_DOUBLE(232.0, dec.m[decoded_find_alias(&dec, GW_ALIAS_TAG_BASE + VOLTAGE)].num);
    /* one message per call: the caller paces the history */
    TEST_ASSERT_EQUAL_size_t(4, gw_node_flush_history(&node, 1));   /* samples left */
    TEST_ASSERT_EQUAL_size_t(0, gw_node_flush_history(&node, 10));
    TEST_ASSERT_EQUAL(6, g_rec.count);
    TEST_ASSERT_EQUAL_UINT32(4, node.sf_sent);

    const rec_msg_t *m = msg(2);
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/DDATA/GW01/Meter1", m->topic);
    TEST_ASSERT_EQUAL_UINT64(2, dec.seq);            /* seq continues after the births */
    TEST_ASSERT_EQUAL(1, dec.count);
    TEST_ASSERT_TRUE(dec.m[0].is_historical);
    TEST_ASSERT_FALSE(dec.m[0].has_name);
    TEST_ASSERT_EQUAL_UINT64(GW_ALIAS_TAG_BASE + VOLTAGE, dec.m[0].alias);
    TEST_ASSERT_EQUAL_UINT64(2000, dec.m[0].timestamp);
    TEST_ASSERT_EQUAL_DOUBLE(231.0, dec.m[0].num);
    msg(3);
    TEST_ASSERT_EQUAL(2, dec.count);                 /* the batch of one poll stays together */
    TEST_ASSERT_EQUAL_UINT64(4000, dec.m[0].timestamp);
    TEST_ASSERT_EQUAL_DOUBLE(232.0, dec.m[0].num);
    TEST_ASSERT_EQUAL_UINT64(GW_ALIAS_TAG_BASE + RUN, dec.m[1].alias);
    TEST_ASSERT_EQUAL(SPB_VAL_BOOL, dec.m[1].kind);  /* the Boolean field of the DBIRTH type */
    TEST_ASSERT_FALSE(dec.m[1].has_datatype);        /* DDATA: no datatype (#159) */
    TEST_ASSERT_TRUE(dec.m[1].is_historical);
    m = msg(4);
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/NDATA/GW01", m->topic);
    TEST_ASSERT_EQUAL_UINT64(GW_ALIAS_DI_BASE, dec.m[0].alias);
    TEST_ASSERT_EQUAL_DOUBLE(1.0, dec.m[0].num);
    TEST_ASSERT_EQUAL_UINT64(4500, dec.m[0].timestamp);
    TEST_ASSERT_TRUE(dec.m[0].is_historical);
    msg(5);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, dec.m[0].num);
    TEST_ASSERT_EQUAL_UINT64(5, dec.seq);

    /* a device that is offline when the session comes back has no DBIRTH: its samples go */
    gw_node_session_down(&node);
    v[0].value = 240.0;
    gw_tagdb_put_device(cfg, 0, v, true);
    gw_node_device_changed(&node, 0);
    gw_tagdb_put_device(cfg, 0, v, false);
    gw_node_device_changed(&node, 0);   /* offline: nothing stored (no DDEATH in the store) */
    TEST_ASSERT_EQUAL_size_t(1, gw_sf_count(&sf));
    recorder_clear();
    gw_node_session_up(&node, 3);
    TEST_ASSERT_EQUAL(1, g_rec.count);  /* NBIRTH only */
    TEST_ASSERT_EQUAL_size_t(0, gw_node_flush_history(&node, 10));
    TEST_ASSERT_EQUAL(1, g_rec.count);
    TEST_ASSERT_EQUAL_UINT32(1, node.sf_discarded);
}

/* Issue #71: an NBIRTH that cannot be published (the connection dropped right after the SUBACK)
 * leaves the node offline, sends no DBIRTH, and lets the next session start cleanly. */
static void test_session_up_needs_the_nbirth(void)
{
    set_meter(true, 230.5, 12.3, 1);
    g_rec.publish_fails = true;
    TEST_ASSERT_FALSE(gw_node_session_up(&node, 3));
    TEST_ASSERT_FALSE(node.online);
    TEST_ASSERT_EQUAL(0, g_rec.count);
    gw_node_device_changed(&node, 0);   /* offline: nothing is attempted */
    TEST_ASSERT_EQUAL(0, g_rec.count);
    g_rec.publish_fails = false;
    TEST_ASSERT_TRUE(gw_node_session_up(&node, 4));
    TEST_ASSERT_EQUAL(2, g_rec.count);   /* NBIRTH and DBIRTH */
    msg(0);
    TEST_ASSERT_EQUAL_UINT64(0, dec.seq);
}

/* SPB-14 with IO-4: pulse counters changed while offline come back as historical UInt64. */
static void test_store_and_forward_counters(void)
{
    static gw_sf_rec_t storage[16];
    gw_sf_t sf;
    gw_sf_init(&sf, storage, 16);
    gw_node_set_store(&node, &sf);
    gw_node_session_up(&node, 1);
    gw_node_session_down(&node);
    uint64_t c[8] = {0};
    c[2] = 0x100000000ull + 5;   /* above 32 bits: exact */
    gw_tagdb_set_counters(c);
    gw_node_io_changed(&node);
    TEST_ASSERT_EQUAL_size_t(1, gw_sf_count(&sf));
    recorder_clear();
    gw_node_session_up(&node, 2);
    gw_node_flush_history(&node, 4);
    const rec_msg_t *m = msg(g_rec.count - 1);
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/NDATA/GW01", m->topic);
    TEST_ASSERT_EQUAL_UINT64(GW_ALIAS_COUNT_BASE + 2, dec.m[0].alias);
    TEST_ASSERT_EQUAL(SPB_VAL_LONG, dec.m[0].kind);   /* UInt64 of the NBIRTH, no datatype */
    TEST_ASSERT_FALSE(dec.m[0].has_datatype);
    TEST_ASSERT_EQUAL_UINT64(0x100000000ull + 5, dec.m[0].u64);
    TEST_ASSERT_TRUE(dec.m[0].is_historical);
}

/* Issue #73: a DBIRTH that is not published (larger than the broker accepts) leaves its device
 * unborn: no DDATA for it; the next change of the device tries the DBIRTH again. */
static void test_dbirth_not_published_leaves_the_device_unborn(void)
{
    set_meter(false, 230.5, 12.3, 1);
    TEST_ASSERT_TRUE(gw_node_session_up(&node, 1));
    TEST_ASSERT_EQUAL(1, g_rec.count);          /* NBIRTH; the meter is offline */
    recorder_clear();
    set_meter(true, 230.5, 12.3, 1);
    g_rec.publish_fails = true;
    gw_node_device_changed(&node, 0);
    TEST_ASSERT_FALSE(node.dev_born[0]);
    g_rec.publish_fails = false;
    set_meter(true, 231.5, 12.3, 1);
    gw_node_device_changed(&node, 0);           /* a DBIRTH again, not a DDATA */
    TEST_ASSERT_EQUAL(1, g_rec.count);
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/DBIRTH/GW01/Meter1", msg(0)->topic);
    TEST_ASSERT_TRUE(node.dev_born[0]);
    /* the failed publish used no seq number (tck-id-payloads-sequence-num-incrementing, #158) */
    TEST_ASSERT_EQUAL_UINT64(1, dec.seq);
}

/* Issue #158: a DBIRTH that does not fit the local payload buffer (6 KB without PSRAM) is
 * "too large": it uses no seq number, the device stays unborn, and the engine does not build
 * it again on each change of the device (only at the next session or rebirth, as #76). */
static void test_dbirth_larger_than_buffer(void)
{
    static char doc[16384];
    static gw_config_t c;
    size_t n = (size_t)snprintf(doc, sizeof(doc), "{\"mqtt\": {\"host\": \"h\"}, \"sparkplug\": "
                                "{\"group_id\": \"Plant1\", \"edge_node_id\": \"GW01\"}, "
                                "\"devices\": [{\"name\": \"Big\", \"unit_id\": 1, \"tags\": [");
    for (int k = 0; k < GW_MAX_TAGS_PER_DEVICE; k++) {
        n += (size_t)snprintf(doc + n, sizeof(doc) - n,
                              "%s{\"name\": \"T%036d%02d\", \"area\": \"input\", \"address\": %d,"
                              " \"type\": \"float32\", \"units\": \"uuuuuuuuuuuuuuu\","
                              " \"eng_low\": -3e38, \"eng_high\": 3e38}",
                              k ? "," : "", 0, k, 2 * k);
    }
    snprintf(doc + n, sizeof(doc) - n, "]}]}");
    gw_cfg_err_t err;
    TEST_ASSERT_TRUE_MESSAGE(gw_config_parse(doc, strlen(doc), &c, &err), err.msg);
    cfg = &c;
    gw_tagdb_reset();
    gw_node_ops_t ops = recorder_ops();
    const gw_node_info_t info = {.firmware = "f", .hardware = "h", .reset_reason = "r"};
    gw_node_init(&node, cfg, &ops, &info);
    static uint8_t small[6144];   /* CONFIG_GW_SPB_PAYLOAD_INTERNAL */
    gw_node_set_buffer(&node, small, sizeof(small));
    static gw_tag_value_t v[GW_MAX_TAGS_PER_DEVICE];
    for (int k = 0; k < GW_MAX_TAGS_PER_DEVICE; k++) {
        v[k] = (gw_tag_value_t){.value = k, .ts_ms = 1000, .valid = true};
    }
    gw_tagdb_put_device(cfg, 0, v, true);
    static char log[4096];
    gw_port_log_capture(log, sizeof(log));
    TEST_ASSERT_TRUE(gw_node_session_up(&node, 1));
    TEST_ASSERT_EQUAL(1, g_rec.count);          /* the NBIRTH fits, the DBIRTH does not */
    TEST_ASSERT_FALSE(node.dev_born[0]);
    TEST_ASSERT_TRUE(node.dev_too_large[0]);
    TEST_ASSERT_EQUAL_UINT8(1, node.seq);       /* the DBIRTH used no seq number */
    TEST_ASSERT_NOT_NULL(strstr(log, "DBIRTH Big: payload does not fit the local buffer of 6144"));
    /* changes of the device: no DBIRTH built again, no DDATA */
    log[0] = '\0';
    for (int i = 0; i < 5; i++) {
        v[0].value = 100 + i;
        gw_tagdb_put_device(cfg, 0, v, true);
        gw_node_device_changed(&node, 0);
    }
    TEST_ASSERT_NULL(strstr(log, "does not fit"));
    TEST_ASSERT_EQUAL(1, g_rec.count);
    TEST_ASSERT_EQUAL_UINT8(1, node.seq);
    /* the next message gets the seq that follows the NBIRTH */
    gw_node_set_firmware_status(&node, "refused: x", 0);
    TEST_ASSERT_EQUAL(2, g_rec.count);
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/NDATA/GW01", msg(1)->topic);
    TEST_ASSERT_EQUAL_UINT64(1, dec.seq);
    /* a rebirth tries every birth once */
    log[0] = '\0';
    TEST_ASSERT_TRUE(gw_node_rebirth(&node));
    TEST_ASSERT_NOT_NULL(strstr(log, "DBIRTH Big: payload does not fit"));
    TEST_ASSERT_EQUAL_UINT8(1, node.seq);
    gw_port_log_capture(NULL, 0);
}

/* FW-1: NBIRTH declares the update command and its state; an NCMD with the URL reaches the
 * firmware_update op (only a non-empty string); state changes go out as NDATA. */
static void test_firmware_update_metrics(void)
{
    gw_node_set_firmware_status(&node, "rolled back: 0.2.0", 0);   /* before the session */
    TEST_ASSERT_EQUAL(0, g_rec.count);
    gw_node_session_up(&node, 0);
    msg(0);
    int u = decoded_find_name(&dec, "Node Control/Firmware Update");
    int st = decoded_find_name(&dec, "Firmware Update/State");
    int pr = decoded_find_name(&dec, "Firmware Update/Progress");
    TEST_ASSERT_TRUE(u >= 0 && st >= 0 && pr >= 0);
    TEST_ASSERT_EQUAL(SPB_DT_STRING, dec.m[u].datatype);
    TEST_ASSERT_EQUAL_UINT64(GW_ALIAS_FW_UPDATE, dec.m[u].alias);
    TEST_ASSERT_EQUAL_STRING("", dec.m[u].str);
    TEST_ASSERT_EQUAL_STRING("rolled back: 0.2.0", dec.m[st].str);
    TEST_ASSERT_EQUAL(SPB_DT_UINT32, dec.m[pr].datatype);
    TEST_ASSERT_EQUAL_DOUBLE(0, dec.m[pr].num);

    recorder_clear();
    spb_metric_t m = {.name = "Node Control/Firmware Update", .datatype = SPB_DT_STRING,
                      .v.str = "https://fw.example/gw-0.2.0.bin"};
    send("spBv1.0/Plant1/NCMD/GW01", &m, 1);
    TEST_ASSERT_EQUAL(1, g_rec.fw_updates);
    TEST_ASSERT_EQUAL_STRING("https://fw.example/gw-0.2.0.bin", g_rec.fw_url);
    TEST_ASSERT_EQUAL(0, g_rec.count);          /* the result comes from the update itself */
    /* by alias, but not a string / empty: discarded */
    spb_metric_t b = {.alias = GW_ALIAS_FW_UPDATE, .datatype = SPB_DT_BOOLEAN, .v.num = 1};
    send("spBv1.0/Plant1/NCMD/GW01", &b, 1);
    spb_metric_t e = {.alias = GW_ALIAS_FW_UPDATE, .datatype = SPB_DT_STRING, .v.str = ""};
    send("spBv1.0/Plant1/NCMD/GW01", &e, 1);
    TEST_ASSERT_EQUAL(1, g_rec.fw_updates);

    gw_node_set_firmware_status(&node, "downloading", 10);
    msg(0);
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/NDATA/GW01", g_rec.msgs[0].topic);
    TEST_ASSERT_EQUAL(2, dec.count);
    TEST_ASSERT_EQUAL_UINT64(GW_ALIAS_FW_STATE, dec.m[0].alias);
    TEST_ASSERT_EQUAL_STRING("downloading", dec.m[0].str);
    TEST_ASSERT_EQUAL_UINT64(GW_ALIAS_FW_PROGRESS, dec.m[1].alias);
    TEST_ASSERT_EQUAL_DOUBLE(10, dec.m[1].num);
    gw_node_set_firmware_status(&node, "downloading", 10);   /* unchanged: nothing */
    TEST_ASSERT_EQUAL(1, g_rec.count);
    gw_node_set_firmware_status(&node, "downloading", 20);   /* only what changed */
    msg(1);
    TEST_ASSERT_EQUAL(1, dec.count);
    TEST_ASSERT_EQUAL_UINT64(GW_ALIAS_FW_PROGRESS, dec.m[0].alias);
    /* issue #87: a long state goes out whole (up to GW_NODE_FW_STATE_MAX - 1); longer is cut
     * with a warning in the log */
    char longs[GW_NODE_FW_STATE_MAX + 20];
    memset(longs, 'r', sizeof(longs));
    longs[GW_NODE_FW_STATE_MAX - 1] = '\0';
    gw_node_set_firmware_status(&node, longs, 20);
    msg(2);
    TEST_ASSERT_EQUAL(GW_NODE_FW_STATE_MAX - 1, strlen(dec.m[0].str));
    longs[GW_NODE_FW_STATE_MAX - 1] = 'r';
    longs[0] = 's';   /* a different state, longer than the limit */
    longs[sizeof(longs) - 1] = '\0';
    gw_node_set_firmware_status(&node, longs, 20);
    msg(3);
    TEST_ASSERT_EQUAL(GW_NODE_FW_STATE_MAX - 1, strlen(dec.m[0].str));
}

/* Issue #87: an NCMD with a String metric of @p n characters named "Node Control/Firmware
 * Update", built by hand (the encoder refuses strings longer than SPB_STRING_MAX). */
static size_t raw_fw_ncmd(uint8_t *out, size_t n)
{
    static const char name[] = "Node Control/Firmware Update";
    uint8_t m[400];
    size_t k = 0;
    m[k++] = 0x0a;
    m[k++] = (uint8_t)(sizeof(name) - 1);
    memcpy(m + k, name, sizeof(name) - 1);
    k += sizeof(name) - 1;
    m[k++] = 0x20;
    m[k++] = SPB_DT_STRING;
    m[k++] = 0x7a;
    m[k++] = (uint8_t)(0x80 | (n & 0x7f));
    m[k++] = (uint8_t)(n >> 7);
    memcpy(m + k, "https://", 8);
    memset(m + k + 8, 'a', n - 8);
    k += n;
    size_t j = 0;
    out[j++] = 0x12;
    out[j++] = (uint8_t)(0x80 | (k & 0x7f));
    out[j++] = (uint8_t)(k >> 7);
    memcpy(out + j, m, k);
    return j + k;
}

/* Issue #87: the firmware URL of an NCMD arrives whole up to SPB_STRING_MAX (256) characters;
 * a longer one makes the NCMD undecodable: nothing applied, nothing cut. */
static void test_firmware_url_limit(void)
{
    gw_node_session_up(&node, 0);
    recorder_clear();
    static uint8_t raw[512];
    size_t len = raw_fw_ncmd(raw, 255);
    gw_node_handle_message(&node, "spBv1.0/Plant1/NCMD/GW01", 24, raw, len);
    TEST_ASSERT_EQUAL(1, g_rec.fw_updates);
    TEST_ASSERT_EQUAL(255, strlen(g_rec.fw_url));
    len = raw_fw_ncmd(raw, SPB_STRING_MAX);
    gw_node_handle_message(&node, "spBv1.0/Plant1/NCMD/GW01", 24, raw, len);
    TEST_ASSERT_EQUAL(2, g_rec.fw_updates);
    TEST_ASSERT_EQUAL(SPB_STRING_MAX, strlen(g_rec.fw_url));
    len = raw_fw_ncmd(raw, SPB_STRING_MAX + 1);
    gw_node_handle_message(&node, "spBv1.0/Plant1/NCMD/GW01", 24, raw, len);
    TEST_ASSERT_EQUAL(2, g_rec.fw_updates);
    TEST_ASSERT_EQUAL(0, g_rec.count);
}

/* Issue #76: a DBIRTH too large for the broker is not rebuilt and rejected on every change of
 * its device: one attempt per session (and per rebirth), none in between. */
static void test_dbirth_too_large_not_retried_on_every_change(void)
{
    set_meter(true, 230.5, 12.3, 1);
    g_rec.too_large_topic = "/DBIRTH/";
    TEST_ASSERT_TRUE(gw_node_session_up(&node, 1));
    TEST_ASSERT_EQUAL(1, g_rec.count);          /* NBIRTH only */
    TEST_ASSERT_EQUAL(1, g_rec.too_large);      /* the DBIRTH was tried once */
    TEST_ASSERT_FALSE(node.dev_born[0]);
    /* the DBIRTH that was not sent left no gap: the next message has seq 1 (SPB-5) */
    gw_node_set_firmware_status(&node, "refused: x", 0);
    TEST_ASSERT_EQUAL(2, g_rec.count);
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/NDATA/GW01", msg(1)->topic);
    TEST_ASSERT_EQUAL_UINT64(1, dec.seq);
    gw_node_set_firmware_status(&node, "idle", 0);
    TEST_ASSERT_EQUAL_UINT8(3, node.seq);
    recorder_clear();
    g_rec.too_large_topic = "/DBIRTH/";
    g_rec.too_large = 1;
    for (int i = 0; i < 5; i++) {               /* changes: no DBIRTH rebuilt, no DDATA */
        set_meter(true, 231.5 + i, 12.3, 1);
        gw_node_device_changed(&node, 0);
    }
    TEST_ASSERT_EQUAL(1, g_rec.too_large);
    TEST_ASSERT_EQUAL(0, g_rec.count);
    /* offline and back within the session: still not retried (and no DDEATH: never born) */
    set_meter(false, 231.5, 12.3, 1);
    gw_node_device_changed(&node, 0);
    set_meter(true, 231.5, 12.3, 1);
    gw_node_device_changed(&node, 0);
    TEST_ASSERT_EQUAL(1, g_rec.too_large);
    TEST_ASSERT_EQUAL(0, g_rec.count);
    /* a rebirth repeats every birth: one attempt */
    TEST_ASSERT_TRUE(gw_node_rebirth(&node));
    TEST_ASSERT_EQUAL(2, g_rec.too_large);
    TEST_ASSERT_EQUAL(1, g_rec.count);          /* the second NBIRTH */
    /* a new session where the broker takes it: DBIRTH, then DDATA */
    gw_node_session_down(&node);
    g_rec.too_large_topic = NULL;
    recorder_clear();
    TEST_ASSERT_TRUE(gw_node_session_up(&node, 2));
    TEST_ASSERT_EQUAL(2, g_rec.count);
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/DBIRTH/GW01/Meter1", g_rec.msgs[1].topic);
    TEST_ASSERT_TRUE(node.dev_born[0]);
    set_meter(true, 240.5, 12.3, 1);
    gw_node_device_changed(&node, 0);
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/DDATA/GW01/Meter1", g_rec.msgs[2].topic);
}

/* SPB-14 with MB-6 values: Int64/UInt64 above 2^53 exact, Double and strings come back from
 * the store as they were sampled. */
static void test_store_and_forward_mb6(void)
{
    setup_mb6();
    static gw_sf_rec_t storage[32];
    gw_sf_t sf;
    gw_sf_init(&sf, storage, 32);
    gw_node_set_store(&node, &sf);
    put_mb6(1, 0, 1.0, "AB", false);
    gw_node_session_up(&node, 0);
    gw_node_session_down(&node);
    put_mb6(9007199254740993ull, -9007199254740993ll, 2.5, "XY", true);
    gw_node_device_changed(&node, 0);
    TEST_ASSERT_EQUAL_size_t(6, gw_sf_count(&sf));   /* 4 values + 1 string continuation + Bit */
    recorder_clear();
    gw_node_session_up(&node, 1);
    TEST_ASSERT_EQUAL_size_t(0, gw_node_flush_history(&node, 4));
    msg(2);
    TEST_ASSERT_EQUAL_STRING("spBv1.0/G/DDATA/N/M", g_rec.msgs[2].topic);
    TEST_ASSERT_EQUAL(5, dec.count);
    for (size_t i = 0; i < dec.count; i++) {
        TEST_ASSERT_TRUE(dec.m[i].is_historical);
    }
    TEST_ASSERT_EQUAL_UINT64(9007199254740993ull, dec.m[0].u64);
    TEST_ASSERT_EQUAL(SPB_VAL_LONG, dec.m[0].kind);   /* the fields of the DBIRTH types, */
    TEST_ASSERT_EQUAL_INT64(-9007199254740993ll, (int64_t)dec.m[1].u64);
    TEST_ASSERT_EQUAL_DOUBLE(2.5, dec.m[2].num);
    TEST_ASSERT_EQUAL(SPB_VAL_STRING, dec.m[3].kind);
    for (size_t i = 0; i < dec.count; i++) {
        TEST_ASSERT_FALSE(dec.m[i].has_datatype);    /* with no datatype (#159) */
    }
    TEST_ASSERT_EQUAL_STRING("XY", dec.m[3].str);
    TEST_ASSERT_EQUAL_DOUBLE(1, dec.m[4].num);
}

/* SPB-14 with SPB-16/18/19: a replayed template device comes back as its instance (historical)
 * with historical members by name and their Quality; raw bytes (NUL included) come back whole. */
static void test_store_and_forward_templates_bytes(void)
{
    static const char json[] =
        "{\"mqtt\": {\"host\": \"h\"}, \"sparkplug\": {\"group_id\": \"G\", \"edge_node_id\": \"N\"},"
        " \"devices\": [{\"name\": \"P\", \"unit_id\": 1, \"type\": \"Pump\", \"tags\": ["
        "  {\"name\": \"Speed\", \"area\": \"holding\", \"address\": 0, \"type\": \"uint16\"},"
        "  {\"name\": \"Raw\", \"area\": \"holding\", \"address\": 1, \"type\": \"bytes\","
        "   \"length\": 2}]}]}";
    static gw_config_t c;
    gw_cfg_err_t err;
    TEST_ASSERT_TRUE_MESSAGE(gw_config_parse(json, sizeof(json) - 1, &c, &err), err.msg);
    cfg = &c;
    gw_tagdb_reset();
    gw_node_ops_t ops = recorder_ops();
    const gw_node_info_t info = {.firmware = "f", .hardware = "h", .reset_reason = "r"};
    gw_node_init(&node, cfg, &ops, &info);
    support_node_buffer(&node);
    static gw_sf_rec_t storage[16];
    gw_sf_t sf;
    gw_sf_init(&sf, storage, 16);
    gw_node_set_store(&node, &sf);
    gw_tag_value_t v[2] = {{.value = 10, .ts_ms = 1000, .valid = true},
                           {.ts_ms = 1000, .valid = true}};
    memcpy(v[1].str, "\x00\x01\x02\x03", 4);
    gw_tagdb_put_device(cfg, 0, v, true);
    gw_node_session_up(&node, 0);
    gw_node_session_down(&node);
    v[0].valid = false;                              /* a failed read while offline */
    v[0].ts_ms = v[1].ts_ms = 2000;
    memcpy(v[1].str, "\x00\xff\x00\xee", 4);
    gw_tagdb_put_device(cfg, 0, v, true);
    gw_node_device_changed(&node, 0);
    recorder_clear();
    gw_node_session_up(&node, 1);
    TEST_ASSERT_EQUAL_size_t(0, gw_node_flush_history(&node, 4));
    msg(g_rec.count - 1);
    TEST_ASSERT_EQUAL_STRING("spBv1.0/G/DDATA/N/P", g_rec.msgs[g_rec.count - 1].topic);
    TEST_ASSERT_EQUAL(1, dec.count);
    TEST_ASSERT_EQUAL(SPB_VAL_TEMPLATE, dec.m[0].kind);
    TEST_ASSERT_FALSE(dec.m[0].has_datatype);        /* DDATA: no datatype (#159) */
    TEST_ASSERT_TRUE(dec.m[0].is_historical);
    TEST_ASSERT_EQUAL(2, dec.mem_count);
    TEST_ASSERT_EQUAL_STRING("Speed", dec.mem[0].name);
    TEST_ASSERT_FALSE(dec.mem[0].has_alias);
    TEST_ASSERT_TRUE(dec.mem[0].is_historical);
    TEST_ASSERT_TRUE(dec.mem[0].is_null);
    TEST_ASSERT_EQUAL_INT32(SPB_QUALITY_BAD, dec.mem[0].quality);
    TEST_ASSERT_EQUAL(SPB_VAL_BYTES, dec.mem[1].kind);
    TEST_ASSERT_FALSE(dec.mem[0].has_datatype);
    TEST_ASSERT_FALSE(dec.mem[1].has_datatype);
    TEST_ASSERT_EQUAL(4, dec.mem[1].bytes_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY("\x00\xff\x00\xee", dec.mem[1].bytes, 4);
    TEST_ASSERT_EQUAL_INT32(SPB_QUALITY_GOOD, dec.mem[1].quality);
}

/* SPB-14: a publish that fails keeps the batch for the next attempt. */
static void test_history_kept_when_publish_fails(void)
{
    static gw_sf_rec_t storage[8];
    gw_sf_t sf;
    gw_sf_init(&sf, storage, 8);
    gw_node_set_store(&node, &sf);
    gw_tagdb_set_di(0x02, 100);
    gw_node_io_changed(&node);   /* before the first NBIRTH: nothing to compare with */
    TEST_ASSERT_EQUAL_size_t(0, gw_sf_count(&sf));
    gw_node_session_up(&node, 1);
    gw_node_session_down(&node);
    gw_tagdb_set_di(0x00, 200);
    gw_node_io_changed(&node);
    TEST_ASSERT_EQUAL_size_t(1, gw_sf_count(&sf));
    gw_node_session_up(&node, 2);
    recorder_clear();
    g_rec.publish_fails = true;
    TEST_ASSERT_EQUAL_size_t(1, gw_node_flush_history(&node, 4));
    g_rec.publish_fails = false;
    TEST_ASSERT_EQUAL_size_t(0, gw_node_flush_history(&node, 4));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_nbirth_and_dbirth);
    RUN_TEST(test_offline_device_has_no_dbirth_until_online);
    RUN_TEST(test_ddata_by_exception_with_deadband);
    RUN_TEST(test_seq_wraps_and_is_shared);
    RUN_TEST(test_ncmd_rebirth);
    RUN_TEST(test_ncmd_do_and_reboot);
    RUN_TEST(test_every_metric_has_a_timestamp);
    RUN_TEST(test_pulse_counters);
    RUN_TEST(test_dcmd_writes);
    RUN_TEST(test_dcmd_strict_datatype_and_integer);
    RUN_TEST(test_dcmd_rejected_whole_when_queue_lacks_room);
    RUN_TEST(test_scan_rate);
    RUN_TEST(test_mb6_birth_and_data);
    RUN_TEST(test_mb6_dcmd);
    RUN_TEST(test_session_down_suppresses_publishing);
    RUN_TEST(test_discard_log_shows_alias_only_if_present);
    RUN_TEST(test_do_unknown_published_as_null);
    RUN_TEST(test_properties_and_quality);
    RUN_TEST(test_bytes_tag);
    RUN_TEST(test_templates);
    RUN_TEST(test_store_and_forward_templates_bytes);
    RUN_TEST(test_diagnostics);
    RUN_TEST(test_time_synced);
    RUN_TEST(test_modbus_tcp_table);
    RUN_TEST(test_worst_case_nbirth_fits);
    RUN_TEST(test_command_aborted_mid_payload_applies_nothing);
    RUN_TEST(test_no_store_coalesces);
    RUN_TEST(test_store_and_forward);
    RUN_TEST(test_history_kept_when_publish_fails);
    RUN_TEST(test_store_and_forward_mb6);
    RUN_TEST(test_session_up_needs_the_nbirth);
    RUN_TEST(test_dbirth_not_published_leaves_the_device_unborn);
    RUN_TEST(test_dbirth_too_large_not_retried_on_every_change);
    RUN_TEST(test_dbirth_larger_than_buffer);
    RUN_TEST(test_firmware_update_metrics);
    RUN_TEST(test_firmware_url_limit);
    RUN_TEST(test_store_and_forward_counters);
    return UNITY_END();
}
