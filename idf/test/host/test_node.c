/* QA-1: Sparkplug edge-node behaviour: births, deaths, seq, aliases, report by
 * exception, rebirth and NCMD/DCMD handling (SPB-2..SPB-12). */
#include <string.h>

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
    gw_node_ops_t ops = recorder_ops();
    const gw_node_info_t info = {
        .firmware = "0.1.0+gdeadbee", .hardware = "test-hw", .reset_reason = "task watchdog"};
    gw_node_init(&node, cfg, &ops, &info);
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
    TEST_ASSERT_EQUAL(GW_NODE_BIRTH_METRICS, dec.count);
    int i = decoded_find_name(&dec, "bdSeq");
    TEST_ASSERT_TRUE(i >= 0);
    TEST_ASSERT_EQUAL(SPB_DT_UINT64, dec.m[i].datatype);
    TEST_ASSERT_EQUAL_DOUBLE(7, dec.m[i].num);
    i = decoded_find_name(&dec, "Node Control/Rebirth");
    TEST_ASSERT_TRUE(i >= 0);
    TEST_ASSERT_EQUAL(SPB_DT_BOOLEAN, dec.m[i].datatype);
    TEST_ASSERT_EQUAL_UINT64(GW_ALIAS_REBIRTH, dec.m[i].alias);
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
    /* every birth metric has name, alias and datatype; aliases are unique */
    for (size_t a = 0; a < dec.count; a++) {
        TEST_ASSERT_TRUE(dec.m[a].has_name);
        TEST_ASSERT_NOT_EQUAL(0, dec.m[a].datatype);
        for (size_t b = a + 1; b < dec.count; b++) {
            TEST_ASSERT_NOT_EQUAL(dec.m[a].alias, dec.m[b].alias);
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
        TEST_ASSERT_EQUAL_UINT64(1000, dec.m[i].timestamp);
    }
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
        gw_tagdb_set_di((uint8_t)(i & 1), (uint64_t)i);
        gw_node_io_changed(&node);
    }
    /* 2 births + 300 NDATA, seq continues 0..255,0.. */
    TEST_ASSERT_EQUAL(REC_MAX, g_rec.count);  /* recorder saturates; check via node state */
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

static void send_raw(const char *topic, const raw_metric_t *m, size_t n)
{
    size_t len = raw_payload(cmd, sizeof(cmd), m, n);
    gw_node_handle_message(&node, topic, strlen(topic), cmd, len);
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

/* Issue #10: nanopb aborts on a name or string longer than 63 characters; the metrics
 * decoded before it must not be applied either. */
static void test_command_aborted_mid_payload_applies_nothing(void)
{
    static const char long_name[] =
        "A_metric_name_that_is_much_longer_than_the_sixty_three_characters_accepted";
    static const char long_str[] =
        "A string value that is much longer than the sixty three characters accepted";
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
    TEST_ASSERT_EQUAL(11, dec.m[1].datatype);
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
    RUN_TEST(test_dcmd_writes);
    RUN_TEST(test_session_down_suppresses_publishing);
    RUN_TEST(test_discard_log_shows_alias_only_if_present);
    RUN_TEST(test_do_unknown_published_as_null);
    RUN_TEST(test_command_aborted_mid_payload_applies_nothing);
    RUN_TEST(test_no_store_coalesces);
    RUN_TEST(test_store_and_forward);
    RUN_TEST(test_history_kept_when_publish_fails);
    return UNITY_END();
}
