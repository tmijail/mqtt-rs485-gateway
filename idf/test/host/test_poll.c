/* QA-1: poll engine against a simulated slave: decoding, offline detection
 * (MB-9), scheduling (MB-10) and DCMD writes with read-back (SPB-10, MB-4). */
#include <math.h>
#include <string.h>

#include "gw_conv.h"
#include "gw_poll.h"
#include "gw_port.h"
#include "gw_tagdb.h"
#include "spb.h"
#include "support.h"
#include "unity.h"

static const gw_config_t *cfg;
static sim_slave_t s;
static mb_master_t mb;
static gw_poller_t p;
static gw_tag_value_t vals[GW_MAX_TAGS_PER_DEVICE];

static void setup_with(bool force_multi)
{
    cfg = support_spec_config();
    gw_tagdb_reset();
    sim_reset(&s, 1);
    s.input[0] = 0x4366;   /* Voltage float32 ABCD = 230.5 */
    s.input[1] = 0x8000;
    s.holding[10] = 123;   /* Setpoint raw, scale 0.1 */
    s.coils[0] = true;     /* Run */
    mb_transport_t t = sim_transport(&s);
    mb_master_cfg_t mc = {.baud = cfg->rs485.baud, .response_timeout_ms = cfg->rs485.response_timeout_ms,
                          .retries = cfg->rs485.retries,
                          .inter_frame_delay_ms = cfg->rs485.inter_frame_delay_ms};
    mb_master_init(&mb, &t, &mc);
    gw_poller_opts_t o = {.merge_reads = true, .max_gap = 0, .force_multi_write = force_multi,
                          .offline_probe_max_ms = 10000};
    gw_poller_init(&p, cfg, &mb, &o, 0);
}

void setUp(void)
{
    setup_with(false);
}
void tearDown(void)
{
    gw_port_fake_time(0);
}

static bool online(void)
{
    bool on;
    gw_tagdb_get_device(cfg, 0, vals, &on);
    return on;
}

static void test_poll_decodes_values(void)
{
    TEST_ASSERT_EQUAL(GW_DEV_WENT_ONLINE, gw_poller_poll(&p, 0, 0));
    TEST_ASSERT_EQUAL(3, s.requests);  /* coil, holding, input blocks */
    TEST_ASSERT_TRUE(online());
    TEST_ASSERT_TRUE(vals[0].valid);
    TEST_ASSERT_EQUAL_DOUBLE(230.5, vals[0].value);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 12.3, vals[1].value);
    TEST_ASSERT_EQUAL_DOUBLE(1, vals[2].value);
    TEST_ASSERT_NOT_EQUAL(0, vals[0].ts_ms);
}

static void test_offline_after_n_cycles_and_back(void)
{
    gw_poller_poll(&p, 0, 0);
    s.offline = true;
    unsigned before = s.requests;
    TEST_ASSERT_EQUAL(GW_DEV_NO_CHANGE, gw_poller_poll(&p, 0, 1000));
    /* the cycle stops at the first unanswered request: 1 try + 2 retries, not 3 blocks x 3 */
    TEST_ASSERT_EQUAL(before + 3, s.requests);
    TEST_ASSERT_TRUE(online());
    TEST_ASSERT_EQUAL(GW_DEV_NO_CHANGE, gw_poller_poll(&p, 0, 2000));
    TEST_ASSERT_EQUAL(GW_DEV_WENT_OFFLINE, gw_poller_poll(&p, 0, 3000)); /* offline_after = 3 */
    TEST_ASSERT_FALSE(online());
    TEST_ASSERT_FALSE(vals[0].valid);
    TEST_ASSERT_EQUAL(1, gw_poller_offline_count(&p));
    s.offline = false;
    TEST_ASSERT_EQUAL(GW_DEV_WENT_ONLINE, gw_poller_poll(&p, 0, 4000));
    TEST_ASSERT_TRUE(online());
    TEST_ASSERT_TRUE(vals[0].valid);
    TEST_ASSERT_EQUAL(0, gw_poller_offline_count(&p));
}

static void test_exception_keeps_device_online(void)
{
    gw_poller_poll(&p, 0, 0);
    s.exception = 2;
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL(GW_DEV_NO_CHANGE, gw_poller_poll(&p, 0, (uint32_t)(1000 * (i + 1))));
    }
    TEST_ASSERT_TRUE(online());
    TEST_ASSERT_FALSE(vals[0].valid);
    TEST_ASSERT_EQUAL(15, mb.stats.exception);
}

/* Review M1: one block unanswered while the others answer. The cycle fails, the unread
 * tags are not refreshed, and the device goes offline after offline_after such cycles. */
static void test_unanswered_block_fails_the_cycle(void)
{
    /* Issue #66: a fake clock that moves on every cycle, so a re-stamped unread tag is caught
     * every time; the reference is read back from gw_tagdb after the first cycle (it used to
     * be the copy left in vals by the previous test, equal only within the same millisecond). */
    const uint64_t t0 = 1762171200000ull;
    gw_port_fake_time(t0);
    TEST_ASSERT_EQUAL(GW_DEV_WENT_ONLINE, gw_poller_poll(&p, 0, 0));
    TEST_ASSERT_TRUE(online());
    gw_tag_value_t voltage = vals[0];
    TEST_ASSERT_EQUAL_UINT64(t0, voltage.ts_ms);
    s.mute_fc = MB_FC_READ_INPUT_REGISTERS;   /* Voltage's block (the last one) */
    s.holding[10] = 200;                      /* Setpoint, still answered */
    s.input[0] = 0x4300;                      /* Voltage changes to 128.0, but is never read */
    s.input[1] = 0x0000;
    for (int cycle = 1; cycle <= 2; cycle++) {
        gw_port_fake_time(t0 + 1000u * (uint64_t)cycle);
        TEST_ASSERT_EQUAL(GW_DEV_NO_CHANGE, gw_poller_poll(&p, 0, (uint32_t)(1000 * cycle)));
        TEST_ASSERT_TRUE(online());
        TEST_ASSERT_EQUAL_UINT64(t0 + 1000u * (uint64_t)cycle, vals[1].ts_ms);  /* re-read */
        TEST_ASSERT_DOUBLE_WITHIN(1e-9, 20.0, vals[1].value);   /* answered block refreshed */
        TEST_ASSERT_TRUE(vals[1].valid);
        /* not read: neither a new value nor a new timestamp */
        TEST_ASSERT_EQUAL_DOUBLE(voltage.value, vals[0].value);
        TEST_ASSERT_EQUAL_UINT64(voltage.ts_ms, vals[0].ts_ms);
    }
    TEST_ASSERT_EQUAL(GW_DEV_WENT_OFFLINE, gw_poller_poll(&p, 0, 3000));  /* offline_after = 3 */
    TEST_ASSERT_FALSE(online());
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_FALSE(vals[i].valid);
    }
    /* still one block short: no way back online */
    TEST_ASSERT_EQUAL(GW_DEV_NO_CHANGE, gw_poller_poll(&p, 0, 4000));
    TEST_ASSERT_FALSE(online());
    s.mute_fc = 0;
    TEST_ASSERT_EQUAL(GW_DEV_WENT_ONLINE, gw_poller_poll(&p, 0, 5000));
    TEST_ASSERT_TRUE(online());
    TEST_ASSERT_TRUE(vals[0].valid);
    TEST_ASSERT_EQUAL_DOUBLE(128.0, vals[0].value);
}

static void test_unanswered_block_skips_the_rest_of_the_cycle(void)
{
    gw_poller_poll(&p, 0, 0);
    s.mute_fc = MB_FC_READ_HOLDING_REGISTERS;  /* middle block: coil, holding, input */
    unsigned before = s.requests;
    gw_poller_poll(&p, 0, 1000);
    TEST_ASSERT_EQUAL(before + 1 + 3, s.requests);  /* coil once, holding 1 + 2 retries, no input */
}

static void test_device_counters(void)
{
    gw_dev_stats_t st;
    gw_poller_poll(&p, 0, 0);                 /* 3 blocks answered */
    s.mute_fc = MB_FC_READ_INPUT_REGISTERS;
    gw_poller_poll(&p, 0, 1000);              /* 2 answered, 1 x (1 + 2 retries) timeouts */
    s.mute_fc = 0;
    s.exception = 2;
    gw_poller_poll(&p, 0, 2000);              /* 3 exceptions, not retried */
    gw_tagdb_get_stats(0, &st);
    TEST_ASSERT_EQUAL_UINT32(3, st.cycles);
    TEST_ASSERT_EQUAL_UINT32(1, st.failed_cycles);
    TEST_ASSERT_EQUAL_UINT32(5, st.bus.ok);
    TEST_ASSERT_EQUAL_UINT32(3, st.bus.timeout);
    TEST_ASSERT_EQUAL_UINT32(3, st.bus.exception);
    s.exception = 0;
    TEST_ASSERT_EQUAL(GW_WRITE_OK, gw_poller_write(&p, 1, 1.0));  /* write + read-back */
    gw_tagdb_get_stats(0, &st);
    TEST_ASSERT_EQUAL_UINT32(7, st.bus.ok);
    TEST_ASSERT_EQUAL_UINT32(3, st.cycles);
}

static void test_scheduling(void)
{
    uint32_t wait;
    TEST_ASSERT_EQUAL(0, gw_poller_next(&p, 0, &wait));
    gw_poller_poll(&p, 0, 0);
    TEST_ASSERT_EQUAL(-1, gw_poller_next(&p, 10, &wait));
    TEST_ASSERT_EQUAL(990, wait);
    TEST_ASSERT_EQUAL(0, gw_poller_next(&p, 1000, &wait));
    gw_poller_poll(&p, 0, 1000);
    TEST_ASSERT_EQUAL(-1, gw_poller_next(&p, 1500, &wait));
    TEST_ASSERT_EQUAL(500, wait);
    gw_poller_poll(&p, 0, 9000);  /* overrun: re-anchored, no burst */
    TEST_ASSERT_EQUAL(-1, gw_poller_next(&p, 9000, &wait));
    TEST_ASSERT_EQUAL(1000, wait);
}

static void test_write_setpoint_fc06(void)
{
    /* SPEC acceptance 4: DCMD Setpoint=12.3 writes 123 to holding 10 with FC06 */
    gw_poller_poll(&p, 0, 0);
    s.holding[10] = 0;
    TEST_ASSERT_EQUAL(GW_WRITE_OK, gw_poller_write(&p, 1, 12.3));
    TEST_ASSERT_EQUAL(123, s.holding[10]);
    TEST_ASSERT_EQUAL(MB_FC_WRITE_SINGLE_REGISTER, s.last_write_fc);
    TEST_ASSERT_EQUAL(MB_FC_READ_HOLDING_REGISTERS, s.last_fc);  /* read-back */
    gw_tag_value_t v;
    gw_tagdb_get_tag(1, &v);
    TEST_ASSERT_TRUE(v.valid);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 12.3, v.value);

    TEST_ASSERT_EQUAL(GW_WRITE_OK, gw_poller_write(&p, 2, 0));
    TEST_ASSERT_FALSE(s.coils[0]);
    TEST_ASSERT_EQUAL(MB_FC_WRITE_SINGLE_COIL, s.last_write_fc);

    TEST_ASSERT_EQUAL(GW_WRITE_NOT_WRITABLE, gw_poller_write(&p, 0, 1));
    unsigned before = s.requests;
    TEST_ASSERT_EQUAL(GW_WRITE_UNKNOWN_TAG, gw_poller_write(&p, 3, 1));   /* issue #13 */
    TEST_ASSERT_EQUAL(before, s.requests);
    TEST_ASSERT_EQUAL(GW_WRITE_BAD_VALUE, gw_poller_write(&p, 1, 4000));  /* raw 40000 > int16 */
    s.offline = true;
    TEST_ASSERT_EQUAL(GW_WRITE_BUS_ERROR, gw_poller_write(&p, 1, 1));
}

/* Issue #7: NaN != 0.0, so a NaN on a coil used to write ON. */
static void test_write_coil_rejects_nan_and_inf(void)
{
    s.coils[0] = false;
    const double bad[] = {NAN, INFINITY, -INFINITY};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL(GW_WRITE_BAD_VALUE, gw_poller_write(&p, 2, bad[i]));
    }
    TEST_ASSERT_EQUAL(0, s.requests);   /* nothing reached the bus */
    TEST_ASSERT_FALSE(s.coils[0]);
    setup_with(true);                   /* FC15 path too */
    TEST_ASSERT_EQUAL(GW_WRITE_BAD_VALUE, gw_poller_write(&p, 2, NAN));
    TEST_ASSERT_EQUAL(0, s.requests);
    /* registers already rejected them */
    TEST_ASSERT_EQUAL(GW_WRITE_BAD_VALUE, gw_poller_write(&p, 1, NAN));
    TEST_ASSERT_EQUAL(GW_WRITE_BAD_VALUE, gw_poller_write(&p, 1, INFINITY));
}

static void test_write_forced_multi(void)
{
    setup_with(true);
    TEST_ASSERT_EQUAL(GW_WRITE_OK, gw_poller_write(&p, 1, -1.5));
    TEST_ASSERT_EQUAL(MB_FC_WRITE_MULTIPLE_REGISTERS, s.last_write_fc);
    TEST_ASSERT_EQUAL_HEX16((uint16_t)-15, s.holding[10]);
    TEST_ASSERT_EQUAL(GW_WRITE_OK, gw_poller_write(&p, 2, 1));
    TEST_ASSERT_EQUAL(MB_FC_WRITE_MULTIPLE_COILS, s.last_write_fc);
    TEST_ASSERT_TRUE(s.coils[0]);
}

/* #197: devices[].write_multiple replaces the build's setting (force_multi_write) for one
 * device: true = FC16 / FC15 also for one register or coil, false = FC06 / FC05. */
static void poller_for(const gw_config_t *c, bool force_multi)
{
    cfg = c;
    gw_tagdb_reset();
    sim_reset(&s, 1);
    mb_transport_t t = sim_transport(&s);
    mb_master_cfg_t mc = {.baud = 9600, .response_timeout_ms = 200, .retries = 2,
                          .inter_frame_delay_ms = 5};
    mb_master_init(&mb, &t, &mc);
    gw_poller_opts_t o = {.merge_reads = true, .force_multi_write = force_multi,
                          .offline_probe_max_ms = 10000};
    gw_poller_init(&p, cfg, &mb, &o, 0);
}

static void test_write_multiple_per_device(void)
{
    static gw_config_t c;
    c = *support_spec_config();
    TEST_ASSERT_EQUAL_INT8(GW_CFG_BUILD_DEFAULT, c.devices[0].write_multiple);   /* no key */
    c.devices[0].write_multiple = 1;                 /* true on a build without the option */
    poller_for(&c, false);
    TEST_ASSERT_EQUAL(GW_WRITE_OK, gw_poller_write(&p, 1, 12.3));
    TEST_ASSERT_EQUAL(MB_FC_WRITE_MULTIPLE_REGISTERS, s.last_write_fc);
    TEST_ASSERT_EQUAL(123, s.holding[10]);
    TEST_ASSERT_EQUAL(GW_WRITE_OK, gw_poller_write(&p, 2, 1));
    TEST_ASSERT_EQUAL(MB_FC_WRITE_MULTIPLE_COILS, s.last_write_fc);
    TEST_ASSERT_TRUE(s.coils[0]);

    c.devices[0].write_multiple = 0;                 /* false on a build with the option */
    poller_for(&c, true);
    TEST_ASSERT_EQUAL(GW_WRITE_OK, gw_poller_write(&p, 1, 4.5));
    TEST_ASSERT_EQUAL(MB_FC_WRITE_SINGLE_REGISTER, s.last_write_fc);
    TEST_ASSERT_EQUAL(GW_WRITE_OK, gw_poller_write(&p, 2, 1));
    TEST_ASSERT_EQUAL(MB_FC_WRITE_SINGLE_COIL, s.last_write_fc);

    /* the key in the document */
    static const char doc[] =
        "{\"mqtt\": {\"host\": \"h\"}, \"sparkplug\": {\"group_id\": \"G\", \"edge_node_id\": \"N\"},"
        " \"devices\": [{\"name\": \"A\", \"unit_id\": 1, \"write_multiple\": true, \"tags\": ["
        "  {\"name\": \"B3\", \"area\": \"holding\", \"address\": 20, \"type\": \"bool\","
        "   \"bit_offset\": 3, \"writable\": true}]},"
        "  {\"name\": \"B\", \"unit_id\": 1, \"write_multiple\": false, \"tags\": ["
        "  {\"name\": \"R\", \"area\": \"holding\", \"address\": 30, \"type\": \"uint16\","
        "   \"writable\": true}]}]}";
    static gw_config_t d;
    gw_cfg_err_t err;
    TEST_ASSERT_TRUE_MESSAGE(gw_config_parse(doc, sizeof(doc) - 1, &d, &err), err.msg);
    TEST_ASSERT_EQUAL_INT8(1, d.devices[0].write_multiple);
    TEST_ASSERT_EQUAL_INT8(0, d.devices[1].write_multiple);
    poller_for(&d, false);
    s.holding[20] = 0x0100;
    TEST_ASSERT_EQUAL(GW_WRITE_OK, gw_poller_write(&p, 0, 1));    /* a register bit: FC03 + FC16 */
    TEST_ASSERT_EQUAL(MB_FC_WRITE_MULTIPLE_REGISTERS, s.last_write_fc);
    TEST_ASSERT_EQUAL_HEX16(0x0108, s.holding[20]);
    poller_for(&d, true);
    TEST_ASSERT_EQUAL(GW_WRITE_OK, gw_poller_write(&p, 1, 7));
    TEST_ASSERT_EQUAL(MB_FC_WRITE_SINGLE_REGISTER, s.last_write_fc);
    TEST_ASSERT_EQUAL(7, s.holding[30]);
}

/* ---- issue #4: offline devices are probed with a backoff ---- */

static uint32_t sim_now_ms(void)
{
    return (uint32_t)(s.clock_us / 1000u);
}

/* An offline device gets one attempt per probe, at 2, 4, 8, 10, 10 ... s (poll 1 s, cap 10 s),
 * and is back online with the first probe it answers. */
static void test_offline_device_is_probed_with_backoff(void)
{
    gw_poller_poll(&p, 0, 0);          /* answered: online, then the slave goes away */
    s.offline = true;
    uint32_t now = 1000;
    for (int i = 0; i < 3; i++, now += 1000) {
        gw_poller_poll(&p, 0, now);   /* was online: 3 full cycles (1 try + 2 retries) */
    }
    TEST_ASSERT_EQUAL(3 + 9, s.requests);
    TEST_ASSERT_FALSE(online());
    uint32_t wait;
    const uint32_t expect[] = {2000, 4000, 8000, 10000, 10000, 10000};
    now -= 1000;                      /* start of the cycle that declared it offline */
    for (size_t i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
        TEST_ASSERT_EQUAL(-1, gw_poller_next(&p, now, &wait));
        TEST_ASSERT_EQUAL_UINT32(expect[i], wait);
        now += wait;
        TEST_ASSERT_EQUAL(0, gw_poller_next(&p, now, &wait));
        unsigned before = s.requests;
        TEST_ASSERT_EQUAL(GW_DEV_NO_CHANGE, gw_poller_poll(&p, 0, now));
        TEST_ASSERT_EQUAL(before + 1, s.requests);   /* a single attempt, no retries */
    }
    /* the slave answers again: the next probe completes the cycle and brings it back */
    s.offline = false;
    TEST_ASSERT_EQUAL(-1, gw_poller_next(&p, now, &wait));
    TEST_ASSERT_EQUAL_UINT32(10000, wait);           /* bound on the way back: cap + one cycle */
    now += wait;
    unsigned before = s.requests;
    TEST_ASSERT_EQUAL(GW_DEV_WENT_ONLINE, gw_poller_poll(&p, 0, now));
    TEST_ASSERT_EQUAL(before + 3, s.requests);       /* probe, then the rest of the cycle */
    TEST_ASSERT_TRUE(online());
    TEST_ASSERT_TRUE(vals[0].valid);
    /* online again: normal period and retries */
    TEST_ASSERT_EQUAL(-1, gw_poller_next(&p, now, &wait));
    TEST_ASSERT_EQUAL_UINT32(1000, wait);
}

/* Issue #61: a device that never answered since boot is offline from the start: one attempt
 * per probe from its very first cycle, with the backoff starting at the first failure. */
static void test_never_seen_device_is_probed_from_the_start(void)
{
    s.offline = true;
    uint32_t now = 0, wait;
    const uint32_t expect[] = {2000, 4000, 8000, 10000, 10000};
    TEST_ASSERT_EQUAL(0, gw_poller_next(&p, now, &wait));
    TEST_ASSERT_EQUAL(GW_DEV_NO_CHANGE, gw_poller_poll(&p, 0, now));
    TEST_ASSERT_EQUAL(1, s.requests);                /* no retries, not 3 full cycles */
    for (size_t i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
        TEST_ASSERT_EQUAL(-1, gw_poller_next(&p, now, &wait));
        TEST_ASSERT_EQUAL_UINT32(expect[i], wait);
        now += wait;
        unsigned before = s.requests;
        TEST_ASSERT_EQUAL(GW_DEV_NO_CHANGE, gw_poller_poll(&p, 0, now));
        TEST_ASSERT_EQUAL(before + 1, s.requests);
    }
    TEST_ASSERT_FALSE(online());
    TEST_ASSERT_EQUAL(1, gw_poller_offline_count(&p));
    /* first answer: complete cycle, online (DBIRTH) */
    s.offline = false;
    now += 10000;
    TEST_ASSERT_EQUAL(GW_DEV_WENT_ONLINE, gw_poller_poll(&p, 0, now));
    TEST_ASSERT_TRUE(online());
    /* once seen, losing it again takes offline_after full cycles (MB-9) */
    s.offline = true;
    unsigned before = s.requests;
    TEST_ASSERT_EQUAL(GW_DEV_NO_CHANGE, gw_poller_poll(&p, 0, now + 1000));
    TEST_ASSERT_EQUAL(before + 3, s.requests);
}

/* An answered probe continues the cycle; if a later block stays unanswered the device
 * (here one that never was online) stays offline and keeps backing off. */
static void test_probe_answered_but_cycle_incomplete_stays_offline(void)
{
    s.offline = true;
    gw_poller_poll(&p, 0, 0);                 /* never seen: probed, next in 2 s */
    s.offline = false;
    s.mute_fc = MB_FC_READ_INPUT_REGISTERS;   /* probe (coils) answered, a later block is not */
    TEST_ASSERT_EQUAL(GW_DEV_NO_CHANGE, gw_poller_poll(&p, 0, 2000));
    TEST_ASSERT_FALSE(online());
    uint32_t wait;
    TEST_ASSERT_EQUAL(-1, gw_poller_next(&p, 2000, &wait));
    TEST_ASSERT_EQUAL_UINT32(4000, wait);     /* still backing off */
}

/* Four devices on the bus, three of them dead: the healthy one keeps its 1 s period. */
static const char k_four_devices[] =
    "{\"mqtt\": {\"host\": \"h\"}, \"sparkplug\": {\"group_id\": \"G\", \"edge_node_id\": \"N\"},"
    " \"rs485\": {\"baud\": 9600, \"response_timeout_ms\": 200, \"retries\": 2},"
    " \"devices\": ["
    "  {\"name\": \"Alive\", \"unit_id\": 1, \"poll_ms\": 1000, \"offline_after\": 3, \"tags\":"
    "   [{\"name\": \"T\", \"area\": \"holding\", \"address\": 0, \"type\": \"uint16\"}]},"
    "  {\"name\": \"Dead2\", \"unit_id\": 2, \"poll_ms\": 1000, \"offline_after\": 3, \"tags\":"
    "   [{\"name\": \"T\", \"area\": \"holding\", \"address\": 0, \"type\": \"uint16\"}]},"
    "  {\"name\": \"Dead3\", \"unit_id\": 3, \"poll_ms\": 1000, \"offline_after\": 3, \"tags\":"
    "   [{\"name\": \"T\", \"area\": \"holding\", \"address\": 0, \"type\": \"uint16\"}]},"
    "  {\"name\": \"Dead4\", \"unit_id\": 4, \"poll_ms\": 1000, \"offline_after\": 3, \"tags\":"
    "   [{\"name\": \"T\", \"area\": \"holding\", \"address\": 0, \"type\": \"uint16\"}]}"
    "]}";

/* Run the bus owner's loop on the simulated clock for @p ms; returns the healthy device's
 * longest interval between polls after @p warmup_ms, and its poll count in *polls. */
static uint32_t run_bus(uint32_t ms, uint32_t warmup_ms, unsigned *polls)
{
    uint32_t last = UINT32_MAX, worst = 0;
    *polls = 0;
    while (sim_now_ms() < ms) {
        uint32_t now = sim_now_ms(), wait;
        int dev = gw_poller_next(&p, now, &wait);
        if (dev < 0) {
            s.clock_us += (uint64_t)wait * 1000u;
            continue;
        }
        if (dev == 0) {
            if (last != UINT32_MAX && now >= warmup_ms && now - last > worst) {
                worst = now - last;
            }
            if (now >= warmup_ms) {
                (*polls)++;
            }
            last = now;
        }
        gw_poller_poll(&p, (uint16_t)dev, now);
    }
    return worst;
}

static void test_dead_devices_do_not_delay_the_healthy_one(void)
{
    static gw_config_t four;
    gw_cfg_err_t err;
    TEST_ASSERT_TRUE_MESSAGE(gw_config_parse(k_four_devices, sizeof(k_four_devices) - 1, &four, &err),
                             err.msg);
    cfg = &four;
    gw_tagdb_reset();
    sim_reset(&s, 1);
    mb_transport_t t = sim_transport(&s);
    mb_master_cfg_t mc = {.baud = 9600, .response_timeout_ms = 200, .retries = 2,
                          .inter_frame_delay_ms = 5};
    mb_master_init(&mb, &t, &mc);
    gw_poller_opts_t o = {.merge_reads = true, .offline_probe_max_ms = 10000};
    gw_poller_init(&p, cfg, &mb, &o, 0);

    unsigned polls;
    /* issue #61: never answered, so probed from the start (one attempt, 2, 4, 8, 10 s): the
     * healthy device keeps its period from the first second, no warm-up needed */
    uint32_t worst = run_bus(130000, 0, &polls);
    TEST_ASSERT_EQUAL(3, gw_poller_offline_count(&p));
    /* one poll per second, delayed at most by the three probes */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(129, polls);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(1000 + 3 * (200 + 20), worst);
    gw_dev_stats_t st;
    gw_tagdb_get_stats(1, &st);
    /* one attempt per probe: at 0, 2, 6, 14 s, then every 10 s */
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(4 + 12, st.bus.timeout);
    TEST_ASSERT_EQUAL_UINT32(10000, p.probe_ms[1]);
}

/* #214: a disabled device is never polled (no request on the bus, no time of the bus), stays
 * not online (so no DBIRTH) and does not count as offline. Its unit can be the unit of an enabled
 * device. */
static void test_disabled_device_is_not_polled(void)
{
    static const char doc[] =
        "{\"mqtt\": {\"host\": \"h\"}, \"sparkplug\": {\"group_id\": \"G\", \"edge_node_id\": \"N\"},"
        " \"devices\": ["
        "  {\"name\": \"Off\", \"unit_id\": 1, \"enabled\": false, \"poll_ms\": 100, \"tags\":"
        "   [{\"name\": \"T\", \"area\": \"holding\", \"address\": 40, \"type\": \"uint16\"}]},"
        "  {\"name\": \"On\", \"unit_id\": 1, \"poll_ms\": 1000, \"tags\":"
        "   [{\"name\": \"T\", \"area\": \"holding\", \"address\": 0, \"type\": \"uint16\"}]}"
        "]}";
    static gw_config_t c;
    gw_cfg_err_t err;
    TEST_ASSERT_TRUE_MESSAGE(gw_config_parse(doc, sizeof(doc) - 1, &c, &err), err.msg);
    TEST_ASSERT_FALSE(c.devices[0].enabled);
    TEST_ASSERT_TRUE(c.devices[1].enabled);
    poller_for(&c, false);
    unsigned polls[2] = {0, 0};
    while (sim_now_ms() < 20000) {
        uint32_t now = sim_now_ms(), wait;
        int dev = gw_poller_next(&p, now, &wait);
        if (dev < 0) {
            TEST_ASSERT_TRUE(wait != UINT32_MAX);   /* the enabled device is always due later */
            s.clock_us += (uint64_t)wait * 1000u;
            continue;
        }
        polls[dev]++;
        gw_poller_poll(&p, (uint16_t)dev, now);
    }
    TEST_ASSERT_EQUAL_UINT(0, polls[0]);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(19, polls[1]);
    TEST_ASSERT_EQUAL_UINT(polls[1], s.requests);     /* one FC03 for each poll of "On" only */
    gw_dev_stats_t st;
    gw_tagdb_get_stats(0, &st);
    TEST_ASSERT_EQUAL_UINT32(0, st.cycles);
    TEST_ASSERT_EQUAL_UINT32(0, st.bus.ok + st.bus.timeout);
    TEST_ASSERT_FALSE(gw_tagdb_device_online(0));
    TEST_ASSERT_TRUE(gw_tagdb_device_online(1));
    TEST_ASSERT_EQUAL(0, gw_poller_offline_count(&p));
    /* a direct call does nothing either */
    unsigned before = s.requests;
    TEST_ASSERT_EQUAL(GW_DEV_NO_CHANGE, gw_poller_poll(&p, 0, sim_now_ms()));
    TEST_ASSERT_EQUAL_UINT(before, s.requests);

    /* only disabled devices: nothing is ever due */
    c.devices[1].enabled = false;
    poller_for(&c, false);
    uint32_t wait;
    TEST_ASSERT_EQUAL(-1, gw_poller_next(&p, 0, &wait));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, wait);
    TEST_ASSERT_EQUAL(0, gw_poller_offline_count(&p));
}

/* ---- MB-6: 64-bit types, strings, byte orders and register bits ---- */

static const char k_mb6[] =
    "{\"mqtt\": {\"host\": \"h\"}, \"sparkplug\": {\"group_id\": \"G\", \"edge_node_id\": \"N\"},"
    " \"devices\": [{\"name\": \"M\", \"unit_id\": 1, \"tags\": ["
    "  {\"name\": \"U64\", \"area\": \"holding\", \"address\": 0, \"type\": \"uint64\"},"
    "  {\"name\": \"I64\", \"area\": \"holding\", \"address\": 4, \"type\": \"int64\","
    "   \"word_order\": \"CDAB\", \"writable\": true},"
    "  {\"name\": \"F64\", \"area\": \"input\", \"address\": 0, \"type\": \"float64\","
    "   \"word_order\": \"DCBA\"},"
    "  {\"name\": \"I32\", \"area\": \"input\", \"address\": 4, \"type\": \"int32\","
    "   \"word_order\": \"BADC\"},"
    "  {\"name\": \"Str\", \"area\": \"holding\", \"address\": 10, \"type\": \"string\","
    "   \"length\": 4, \"writable\": true},"
    "  {\"name\": \"B3\", \"area\": \"holding\", \"address\": 20, \"type\": \"bool\","
    "   \"bit_offset\": 3, \"writable\": true},"
    "  {\"name\": \"B15\", \"area\": \"input\", \"address\": 6, \"type\": \"bool\", \"bit_offset\": 15}"
    "]}]}";

static gw_config_t mb6;

static void setup_mb6(bool force_multi)
{
    gw_cfg_err_t err;
    TEST_ASSERT_TRUE_MESSAGE(gw_config_parse(k_mb6, sizeof(k_mb6) - 1, &mb6, &err), err.msg);
    cfg = &mb6;
    gw_tagdb_reset();
    sim_reset(&s, 1);
    /* U64 = 2^53 + 1 (not exact in a double), ABCD */
    s.holding[0] = 0x0020;
    s.holding[3] = 0x0001;
    /* I64 = -2, CDAB: least significant word first */
    s.holding[4] = 0xFFFE;
    s.holding[5] = s.holding[6] = s.holding[7] = 0xFFFF;
    /* F64 = -0.5 = 0xBFE0000000000000, DCBA */
    s.input[3] = 0xE0BF;
    /* I32 = -123456 = 0xFFFE1DC0, BADC */
    s.input[4] = 0xFEFF;
    s.input[5] = 0xC01D;
    /* B15: bit 15 of input 6 */
    s.input[6] = 0x8000;
    memcpy(&s.holding[10], ((const uint16_t[]){0x4142, 0x4300, 0, 0}), 4 * sizeof(uint16_t));
    s.holding[20] = 0x0109;   /* bit 3 set, other bits must survive writes */
    mb_transport_t t = sim_transport(&s);
    mb_master_cfg_t mc = {.baud = 9600, .response_timeout_ms = 200, .retries = 2,
                          .inter_frame_delay_ms = 5};
    mb_master_init(&mb, &t, &mc);
    gw_poller_opts_t o = {.merge_reads = true, .force_multi_write = force_multi,
                          .offline_probe_max_ms = 10000};
    gw_poller_init(&p, cfg, &mb, &o, 0);
}

static void test_mb6_decode(void)
{
    setup_mb6(false);
    TEST_ASSERT_EQUAL(GW_DEV_WENT_ONLINE, gw_poller_poll(&p, 0, 0));
    TEST_ASSERT_TRUE(online());
    TEST_ASSERT_EQUAL_UINT64(9007199254740993ull, vals[0].u64);
    TEST_ASSERT_EQUAL_INT64(-2, vals[1].i64);
    TEST_ASSERT_EQUAL_DOUBLE(-0.5, vals[2].value);
    TEST_ASSERT_EQUAL_DOUBLE(-123456.0, vals[3].value);
    TEST_ASSERT_EQUAL_STRING("ABC", vals[4].str);
    TEST_ASSERT_EQUAL_DOUBLE(1.0, vals[5].value);
    TEST_ASSERT_EQUAL_DOUBLE(1.0, vals[6].value);
    s.input[6] = 0x7FFF;   /* every bit but 15 */
    gw_poller_poll(&p, 0, 1000);
    TEST_ASSERT_TRUE(online());
    TEST_ASSERT_EQUAL_DOUBLE(0.0, vals[6].value);
}

static void test_mb6_writes(void)
{
    setup_mb6(false);
    gw_poller_poll(&p, 0, 0);
    /* int64 beyond 2^53, exact, as one FC16 of 4 registers in CDAB order */
    gw_tag_value_t v = {.i64 = 4611686018427387907ll};   /* 2^62 + 3 */
    TEST_ASSERT_EQUAL(GW_WRITE_OK, gw_poller_write_value(&p, 1, &v));
    TEST_ASSERT_EQUAL(MB_FC_WRITE_MULTIPLE_REGISTERS, s.last_write_fc);
    TEST_ASSERT_EQUAL_HEX16_ARRAY(((const uint16_t[]){0x0003, 0x0000, 0x0000, 0x4000}),
                                  &s.holding[4], 4);
    gw_tag_value_t got;
    gw_tagdb_get_tag(1, &got);
    TEST_ASSERT_TRUE(got.valid);
    TEST_ASSERT_EQUAL_INT64(4611686018427387907ll, got.i64);
    /* string: NUL padded, too long rejected */
    strcpy(v.str, "XYZ12");
    TEST_ASSERT_EQUAL(GW_WRITE_OK, gw_poller_write_value(&p, 4, &v));
    TEST_ASSERT_EQUAL_HEX16_ARRAY(((const uint16_t[]){0x5859, 0x5A31, 0x3200, 0x0000}),
                                  &s.holding[10], 4);
    gw_tagdb_get_tag(4, &got);
    TEST_ASSERT_EQUAL_STRING("XYZ12", got.str);
    strcpy(v.str, "123456789");
    unsigned before = s.requests;
    TEST_ASSERT_EQUAL(GW_WRITE_BAD_VALUE, gw_poller_write_value(&p, 4, &v));
    TEST_ASSERT_EQUAL(before, s.requests);
    /* register bit: read-modify-write, the other bits are kept */
    TEST_ASSERT_EQUAL(GW_WRITE_OK, gw_poller_write(&p, 5, 0));
    TEST_ASSERT_EQUAL_HEX16(0x0101, s.holding[20]);
    TEST_ASSERT_EQUAL(MB_FC_WRITE_SINGLE_REGISTER, s.last_write_fc);
    gw_tagdb_get_tag(5, &got);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, got.value);
    s.holding[20] = 0xF000;          /* the device changed other bits meanwhile */
    TEST_ASSERT_EQUAL(GW_WRITE_OK, gw_poller_write(&p, 5, 1));
    TEST_ASSERT_EQUAL_HEX16(0xF008, s.holding[20]);
    TEST_ASSERT_EQUAL(GW_WRITE_BAD_VALUE, gw_poller_write(&p, 5, NAN));
    /* the read of a bit write fails: nothing is written */
    s.offline = true;
    TEST_ASSERT_EQUAL(GW_WRITE_BUS_ERROR, gw_poller_write(&p, 5, 0));
    TEST_ASSERT_EQUAL_HEX16(0xF008, s.holding[20]);
    s.offline = false;
    /* FC16 forced: the bit write goes as FC16 of one register */
    setup_mb6(true);
    TEST_ASSERT_EQUAL(GW_WRITE_OK, gw_poller_write(&p, 5, 0));
    TEST_ASSERT_EQUAL(MB_FC_WRITE_MULTIPLE_REGISTERS, s.last_write_fc);
    TEST_ASSERT_EQUAL_HEX16(0x0101, s.holding[20]);
}

/* ---- #202: a field of bits of a register (type uint16) ---- */

static const char k_field[] =
    "{\"mqtt\": {\"host\": \"h\"}, \"sparkplug\": {\"group_id\": \"G\", \"edge_node_id\": \"N\"},"
    " \"devices\": [{\"name\": \"M\", \"unit_id\": 1, \"tags\": ["
    "  {\"name\": \"State\", \"area\": \"holding\", \"address\": 20, \"type\": \"uint16\","
    "   \"bit_offset\": 4, \"bit_width\": 3, \"writable\": true},"
    "  {\"name\": \"Hi\", \"area\": \"holding\", \"address\": 20, \"type\": \"uint16\","
    "   \"bit_offset\": 12, \"bit_width\": 4, \"scale\": 0.5},"
    "  {\"name\": \"Low\", \"area\": \"input\", \"address\": 3, \"type\": \"uint16\","
    "   \"bit_offset\": 0, \"bit_width\": 2},"
    "  {\"name\": \"All\", \"area\": \"input\", \"address\": 4, \"type\": \"uint16\","
    "   \"bit_offset\": 0, \"bit_width\": 16}"
    "]}]}";

static gw_config_t field_cfg;

static void setup_field(bool force_multi)
{
    gw_cfg_err_t err;
    TEST_ASSERT_TRUE_MESSAGE(gw_config_parse(k_field, sizeof(k_field) - 1, &field_cfg, &err),
                             err.msg);
    cfg = &field_cfg;
    gw_tagdb_reset();
    sim_reset(&s, 1);
    s.holding[20] = 0xA0D9;   /* bits 4..6 = 0b101 = 5; bits 12..15 = 0xA = 10 */
    s.input[3] = 0xFFFE;      /* bits 0..1 = 0b10 = 2 */
    s.input[4] = 0xFFFF;
    mb_transport_t t = sim_transport(&s);
    mb_master_cfg_t mc = {.baud = 9600, .response_timeout_ms = 200, .retries = 2,
                          .inter_frame_delay_ms = 5};
    mb_master_init(&mb, &t, &mc);
    gw_poller_opts_t o = {.merge_reads = true, .force_multi_write = force_multi,
                          .offline_probe_max_ms = 10000};
    gw_poller_init(&p, cfg, &mb, &o, 0);
}

static void test_bit_field_read(void)
{
    setup_field(false);
    TEST_ASSERT_EQUAL(GW_DEV_WENT_ONLINE, gw_poller_poll(&p, 0, 0));
    TEST_ASSERT_TRUE(online());
    TEST_ASSERT_EQUAL_DOUBLE(5.0, vals[0].value);
    TEST_ASSERT_EQUAL_DOUBLE(5.0, vals[1].value);   /* 10 x 0.5 */
    TEST_ASSERT_EQUAL_DOUBLE(2.0, vals[2].value);
    TEST_ASSERT_EQUAL_DOUBLE(65535.0, vals[3].value);
    TEST_ASSERT_EQUAL(SPB_DT_UINT16, gw_conv_datatype(&field_cfg.tags[0]));
    TEST_ASSERT_EQUAL(SPB_DT_FLOAT, gw_conv_datatype(&field_cfg.tags[1]));
    s.holding[20] = 0x0F8F;   /* bits 4..6 = 0 */
    gw_poller_poll(&p, 0, 1000);
    TEST_ASSERT_TRUE(online());
    TEST_ASSERT_EQUAL_DOUBLE(0.0, vals[0].value);
}

static void test_bit_field_write(void)
{
    setup_field(false);
    gw_poller_poll(&p, 0, 0);
    /* read-modify-write: only the bits 4..6 change */
    TEST_ASSERT_EQUAL(GW_WRITE_OK, gw_poller_write(&p, 0, 3));
    TEST_ASSERT_EQUAL(MB_FC_WRITE_SINGLE_REGISTER, s.last_write_fc);
    TEST_ASSERT_EQUAL_HEX16(0xA0B9, s.holding[20]);
    gw_tag_value_t got;
    gw_tagdb_get_tag(0, &got);
    TEST_ASSERT_TRUE(got.valid);
    TEST_ASSERT_EQUAL_DOUBLE(3.0, got.value);   /* the read-back of the field */
    TEST_ASSERT_EQUAL(GW_WRITE_OK, gw_poller_write(&p, 0, 7));
    TEST_ASSERT_EQUAL_HEX16(0xA0F9, s.holding[20]);
    TEST_ASSERT_EQUAL(GW_WRITE_OK, gw_poller_write(&p, 0, 0));
    TEST_ASSERT_EQUAL_HEX16(0xA089, s.holding[20]);
    /* a value that does not fit 3 bits: no request at all */
    unsigned before = s.requests;
    TEST_ASSERT_EQUAL(GW_WRITE_BAD_VALUE, gw_poller_write(&p, 0, 8));
    TEST_ASSERT_EQUAL(GW_WRITE_BAD_VALUE, gw_poller_write(&p, 0, -1));
    TEST_ASSERT_EQUAL(before, s.requests);
    TEST_ASSERT_EQUAL_HEX16(0xA089, s.holding[20]);
    /* the device changed other bits meanwhile: they stay */
    s.holding[20] = 0x5F0F;
    TEST_ASSERT_EQUAL(GW_WRITE_OK, gw_poller_write(&p, 0, 2));
    TEST_ASSERT_EQUAL_HEX16(0x5F2F, s.holding[20]);
    /* the read of the register fails: nothing is written */
    s.offline = true;
    TEST_ASSERT_EQUAL(GW_WRITE_BUS_ERROR, gw_poller_write(&p, 0, 1));
    s.offline = false;
    TEST_ASSERT_EQUAL_HEX16(0x5F2F, s.holding[20]);
    /* FC16 forced: one register with FC16 */
    setup_field(true);
    TEST_ASSERT_EQUAL(GW_WRITE_OK, gw_poller_write(&p, 0, 1));
    TEST_ASSERT_EQUAL(MB_FC_WRITE_MULTIPLE_REGISTERS, s.last_write_fc);
    TEST_ASSERT_EQUAL_HEX16(0xA099, s.holding[20]);
}

/* ---- MB-12: fast and slow scan ---- */

static const char k_scan[] =
    "{\"mqtt\": {\"host\": \"h\"}, \"sparkplug\": {\"group_id\": \"G\", \"edge_node_id\": \"N\"},"
    " \"devices\": [{\"name\": \"M\", \"unit_id\": 1, \"poll_ms\": 1000, \"slow_poll_ms\": 5000,"
    "  \"tags\": ["
    "  {\"name\": \"Fast\", \"area\": \"holding\", \"address\": 0, \"type\": \"uint16\"},"
    "  {\"name\": \"Slow\", \"area\": \"holding\", \"address\": 1, \"type\": \"uint16\","
    "   \"scan\": \"slow\"},"
    "  {\"name\": \"Slow2\", \"area\": \"input\", \"address\": 0, \"type\": \"uint16\","
    "   \"scan\": \"slow\"}]},"
    " {\"name\": \"OnlySlow\", \"unit_id\": 2, \"poll_ms\": 1000, \"slow_poll_ms\": 4000,"
    "  \"tags\": [{\"name\": \"S\", \"area\": \"holding\", \"address\": 0, \"type\": \"uint16\","
    "   \"scan\": \"slow\"}]}]}";

static gw_config_t scan_cfg;

static void setup_scan(void)
{
    gw_cfg_err_t err;
    TEST_ASSERT_TRUE_MESSAGE(gw_config_parse(k_scan, sizeof(k_scan) - 1, &scan_cfg, &err), err.msg);
    cfg = &scan_cfg;
    gw_tagdb_reset();
    sim_reset(&s, 1);
    s.holding[0] = 10;
    s.holding[1] = 20;
    s.input[0] = 30;
    mb_transport_t t = sim_transport(&s);
    mb_master_cfg_t mc = {.baud = 9600, .response_timeout_ms = 200, .retries = 2,
                          .inter_frame_delay_ms = 5};
    mb_master_init(&mb, &t, &mc);
    gw_poller_opts_t o = {.merge_reads = true, .offline_probe_max_ms = 10000};
    gw_poller_init(&p, cfg, &mb, &o, 0);
}

static void test_scan_classes(void)
{
    setup_scan();
    /* fast and slow tags never share a block, even when contiguous */
    TEST_ASSERT_EQUAL(3, p.plan.dev_block_count[0]);
    TEST_ASSERT_EQUAL(1, p.plan.dev_fast_count[0]);
    TEST_ASSERT_FALSE(p.plan.blocks[0].slow);
    TEST_ASSERT_TRUE(p.plan.blocks[1].slow);
    /* first cycle (offline, never seen): every block, so the DBIRTH has every tag */
    TEST_ASSERT_EQUAL(GW_DEV_WENT_ONLINE, gw_poller_poll(&p, 0, 0));
    TEST_ASSERT_EQUAL(3, s.requests);
    TEST_ASSERT_TRUE(online());
    TEST_ASSERT_EQUAL_DOUBLE(20, vals[1].value);
    uint64_t slow_ts = vals[1].ts_ms;
    /* 1..4 s: fast only */
    s.holding[1] = 21;
    for (uint32_t t = 1000; t <= 4000; t += 1000) {
        unsigned before = s.requests;
        gw_poller_poll(&p, 0, t);
        TEST_ASSERT_EQUAL(before + 1, s.requests);
    }
    TEST_ASSERT_TRUE(online());
    TEST_ASSERT_EQUAL_DOUBLE(20, vals[1].value);         /* not refreshed */
    TEST_ASSERT_EQUAL_UINT64(slow_ts, vals[1].ts_ms);
    /* 5 s: the slow ones are due */
    unsigned before = s.requests;
    gw_poller_poll(&p, 0, 5000);
    TEST_ASSERT_EQUAL(before + 3, s.requests);
    TEST_ASSERT_TRUE(online());
    TEST_ASSERT_EQUAL_DOUBLE(21, vals[1].value);
    /* a fast-only cycle that fails counts for offline detection (MB-9) */
    s.offline = true;
    gw_poller_poll(&p, 0, 6000);
    gw_poller_poll(&p, 0, 7000);
    TEST_ASSERT_EQUAL(GW_DEV_WENT_OFFLINE, gw_poller_poll(&p, 0, 8000));
    /* back: the cycle that brings it online reads the slow blocks too, though not due yet */
    s.offline = false;
    s.input[0] = 31;
    before = s.requests;
    TEST_ASSERT_EQUAL(GW_DEV_WENT_ONLINE, gw_poller_poll(&p, 0, 10000));
    TEST_ASSERT_EQUAL(before + 3, s.requests);
    TEST_ASSERT_TRUE(online());
    TEST_ASSERT_EQUAL_DOUBLE(31, vals[2].value);
}

/* A cycle that fails before the slow blocks leaves them due for the next cycle. */
static void test_slow_blocks_stay_due_after_a_failure(void)
{
    setup_scan();
    gw_poller_poll(&p, 0, 0);
    for (uint32_t t = 1000; t <= 4000; t += 1000) {
        gw_poller_poll(&p, 0, t);
    }
    s.mute_fc = MB_FC_READ_INPUT_REGISTERS;   /* the last slow block */
    unsigned before = s.requests;
    gw_poller_poll(&p, 0, 5000);
    TEST_ASSERT_EQUAL(before + 2 + 3, s.requests);
    s.mute_fc = 0;
    before = s.requests;
    gw_poller_poll(&p, 0, 6000);
    TEST_ASSERT_EQUAL(before + 3, s.requests);  /* still due */
    before = s.requests;
    gw_poller_poll(&p, 0, 7000);
    TEST_ASSERT_EQUAL(before + 1, s.requests);  /* due in 5 s again */
}

/* A device with only slow tags is polled every slow_poll_ms, also when its cycles fail. */
static void test_only_slow_tags(void)
{
    setup_scan();
    sim_reset(&s, 2);
    TEST_ASSERT_EQUAL(GW_DEV_WENT_ONLINE, gw_poller_poll(&p, 1, 0));
    TEST_ASSERT_EQUAL_UINT32(4000, p.next_due_ms[1]);   /* not poll_ms (1 s) */
    TEST_ASSERT_EQUAL(GW_DEV_NO_CHANGE, gw_poller_poll(&p, 1, 4000));
    TEST_ASSERT_EQUAL_UINT32(8000, p.next_due_ms[1]);
    s.offline = true;                                   /* failed while online: no busy loop */
    unsigned before = s.requests;
    TEST_ASSERT_EQUAL(GW_DEV_NO_CHANGE, gw_poller_poll(&p, 1, 8000));
    TEST_ASSERT_EQUAL(before + 3, s.requests);
    TEST_ASSERT_EQUAL_UINT32(12000, p.next_due_ms[1]);
}

/* ---- MB-14: writes served between the requests of a cycle ---- */

static const char k_long[] =
    "{\"mqtt\": {\"host\": \"h\"}, \"sparkplug\": {\"group_id\": \"G\", \"edge_node_id\": \"N\"},"
    " \"devices\": [{\"name\": \"Slow\", \"unit_id\": 1, \"block_reads\": false, \"tags\": ["
    "  {\"name\": \"T0\", \"area\": \"holding\", \"address\": 0, \"type\": \"uint16\", \"writable\": true},"
    "  {\"name\": \"T1\", \"area\": \"holding\", \"address\": 1, \"type\": \"uint16\"},"
    "  {\"name\": \"T2\", \"area\": \"holding\", \"address\": 2, \"type\": \"uint16\"},"
    "  {\"name\": \"T3\", \"area\": \"holding\", \"address\": 3, \"type\": \"uint16\"},"
    "  {\"name\": \"T4\", \"area\": \"holding\", \"address\": 4, \"type\": \"uint16\"},"
    "  {\"name\": \"T5\", \"area\": \"holding\", \"address\": 5, \"type\": \"uint16\"},"
    "  {\"name\": \"T6\", \"area\": \"holding\", \"address\": 6, \"type\": \"uint16\"},"
    "  {\"name\": \"T7\", \"area\": \"holding\", \"address\": 7, \"type\": \"uint16\"},"
    "  {\"name\": \"T8\", \"area\": \"holding\", \"address\": 8, \"type\": \"uint16\"},"
    "  {\"name\": \"T9\", \"area\": \"holding\", \"address\": 9, \"type\": \"uint16\"}]}]}";

static gw_config_t long_cfg;

/* The bus owner's write queue, as seen by the hook: one pending write, queued while the cycle
 * runs (after request @p at of the cycle). */
static struct {
    unsigned queue_after;    /* the write arrives once this many requests went out */
    bool pending, done;
    unsigned cycle_start, served_at;
    uint64_t queued_us, written_us;
} hook;

static void serve(void *ctx)
{
    (void)ctx;
    if (!hook.pending && !hook.done && s.requests - hook.cycle_start >= hook.queue_after) {
        hook.pending = true;            /* a DCMD arrives during this request */
        hook.queued_us = s.clock_us;
        return;                         /* ... and is seen at the next boundary */
    }
    if (hook.pending) {
        hook.pending = false;
        hook.done = true;
        hook.served_at = s.requests - hook.cycle_start;
        hook.written_us = s.clock_us;
        TEST_ASSERT_EQUAL(GW_WRITE_OK, gw_poller_write(&p, 0, 777));
    }
}

static void test_write_served_between_requests(void)
{
    gw_cfg_err_t err;
    TEST_ASSERT_TRUE_MESSAGE(gw_config_parse(k_long, sizeof(k_long) - 1, &long_cfg, &err), err.msg);
    cfg = &long_cfg;
    gw_tagdb_reset();
    sim_reset(&s, 1);
    s.holding[0] = 1;
    mb_transport_t t = sim_transport(&s);
    mb_master_cfg_t mc = {.baud = 9600, .response_timeout_ms = 200, .retries = 2,
                          .inter_frame_delay_ms = 5};
    mb_master_init(&mb, &t, &mc);
    gw_poller_opts_t o = {.merge_reads = true, .offline_probe_max_ms = 10000,
                          .between_requests = serve};
    gw_poller_init(&p, cfg, &mb, &o, 0);
    TEST_ASSERT_EQUAL(10, p.plan.dev_block_count[0]);

    memset(&hook, 0, sizeof(hook));
    hook.queue_after = 2;               /* T0 (holding 0) was already read in this cycle */
    hook.cycle_start = s.requests;
    uint64_t start_us = s.clock_us;
    TEST_ASSERT_EQUAL(GW_DEV_WENT_ONLINE, gw_poller_poll(&p, 0, 0));
    uint64_t cycle_us = s.clock_us - start_us;
    TEST_ASSERT_TRUE(hook.done);
    /* served at the next request boundary, 8 requests before the end of the cycle */
    TEST_ASSERT_EQUAL(3, hook.served_at);
    TEST_ASSERT_EQUAL(10 + 2, s.requests - hook.cycle_start);   /* + write + read-back */
    TEST_ASSERT_EQUAL(777, s.holding[0]);
    /* latency: at most one transaction, not the rest of the cycle */
    uint64_t latency = hook.written_us - hook.queued_us;
    TEST_ASSERT_TRUE_MESSAGE(latency < cycle_us / 5, "write waited for the cycle");
    TEST_ASSERT_LESS_OR_EQUAL_UINT64(40000, latency);      /* one FC03 at 9600 baud + t3.5 */
    /* T0 was read before the write: the cycle publishes the confirmed value, not the stale one */
    TEST_ASSERT_TRUE(online());
    TEST_ASSERT_EQUAL_DOUBLE(777, vals[0].value);
    TEST_ASSERT_EQUAL(-1, p.polling);
}

/* SPB-15: the node's Scan Rate replaces every poll_ms; 0 goes back to them. */
static void test_scan_rate_override(void)
{
    uint32_t wait;
    gw_poller_poll(&p, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(1000, p.next_due_ms[0]);
    gw_poller_set_scan_rate(&p, 3000, 100);              /* longer: after the poll already due */
    TEST_ASSERT_EQUAL_UINT32(1000, p.next_due_ms[0]);
    gw_poller_poll(&p, 0, 1000);
    TEST_ASSERT_EQUAL(-1, gw_poller_next(&p, 1000, &wait));
    TEST_ASSERT_EQUAL_UINT32(3000, wait);
    gw_poller_set_scan_rate(&p, 200, 1500);              /* shorter: at once */
    TEST_ASSERT_EQUAL_UINT32(1700, p.next_due_ms[0]);
    gw_poller_poll(&p, 0, 1700);
    TEST_ASSERT_EQUAL_UINT32(1900, p.next_due_ms[0]);
    gw_poller_set_scan_rate(&p, 0, 1800);                /* back to poll_ms (1 s) */
    gw_poller_poll(&p, 0, 1900);
    TEST_ASSERT_EQUAL_UINT32(2900, p.next_due_ms[0]);
    /* offline probes back off from the rate in force */
    gw_poller_set_scan_rate(&p, 4000, 2000);
    s.offline = true;
    for (uint32_t t = 2900; t < 2900 + 3 * 4000; t += 4000) {
        gw_poller_poll(&p, 0, t);
    }
    TEST_ASSERT_FALSE(online());
    TEST_ASSERT_EQUAL_UINT32(8000, p.probe_ms[0]);       /* 2 x 4000 */
}

/* The slow scan is never faster than the Scan Rate. */
static void test_scan_rate_and_slow_scan(void)
{
    setup_scan();                                        /* poll 1 s, slow 5 s */
    gw_poller_poll(&p, 0, 0);
    gw_poller_set_scan_rate(&p, 8000, 0);
    gw_poller_poll(&p, 0, 1000);                         /* fast only, next in 8 s */
    TEST_ASSERT_EQUAL_UINT32(9000, p.next_due_ms[0]);
    unsigned before = s.requests;
    gw_poller_poll(&p, 0, 9000);                         /* slow due at 5 s: read now */
    TEST_ASSERT_EQUAL(before + 3, s.requests);
    TEST_ASSERT_EQUAL_UINT32(13000, p.slow_due_ms[0]);   /* due at 5 s + max(5 s, 8 s) */
}

/* Issue #69: forwarded Modbus TCP transactions are counted per unit and by the gateway. */
static void test_forwarded_requests_counted(void)
{
    gw_mbap_stats_reset();
    gw_mbap_txn_t txn;
    memset(&txn, 0, sizeof(txn));
    txn.rq = (mb_request_t){.unit = 1, .fc = MB_FC_READ_HOLDING_REGISTERS, .addr = 10, .qty = 1};
    gw_poller_forward(&p, &txn);                  /* Meter1: answered */
    TEST_ASSERT_EQUAL(MB_OK, txn.status);
    s.exception = 2;
    gw_poller_forward(&p, &txn);                  /* the slave's own exception: passed through */
    s.exception = 0;
    s.offline = true;
    gw_poller_forward(&p, &txn);                  /* no answer: 1 + 2 retries, gateway 0B */
    s.offline = false;
    txn.rq.unit = 9;                              /* no device with unit 9 */
    gw_poller_forward(&p, &txn);
    gw_dev_stats_t st;
    gw_tagdb_get_stats(0, &st);
    TEST_ASSERT_EQUAL_UINT32(1, st.tcp.ok);
    TEST_ASSERT_EQUAL_UINT32(1, st.tcp.exception);
    TEST_ASSERT_EQUAL_UINT32(3, st.tcp.timeout);
    TEST_ASSERT_EQUAL_UINT32(0, st.bus.ok + st.bus.timeout + st.bus.exception);   /* polls apart */
    TEST_ASSERT_EQUAL_UINT32(0, st.cycles);
    gw_mbap_stats_t g;
    gw_mbap_stats_get(&g);
    TEST_ASSERT_EQUAL_UINT32(4, g.forwarded);
    TEST_ASSERT_EQUAL_UINT32(2, g.ex_no_answer);  /* the dead request and unit 9 */
    TEST_ASSERT_EQUAL_UINT32(3, g.other_units.timeout);
    TEST_ASSERT_EQUAL_UINT32(0, g.other_units.ok);
    gw_mbap_count_exception(GW_MBAP_EX_ILLEGAL_FUNCTION);
    gw_mbap_count_exception(GW_MBAP_EX_PATH_UNAVAILABLE);
    gw_mbap_stats_add(&(const gw_mbap_stats_t){.accepted = 2, .rejected = 1});
    gw_mbap_stats_get(&g);
    TEST_ASSERT_EQUAL_UINT32(1, g.ex_function);
    TEST_ASSERT_EQUAL_UINT32(1, g.ex_path);
    TEST_ASSERT_EQUAL_UINT32(0, g.ex_value);
    TEST_ASSERT_EQUAL_UINT32(2, g.accepted);
    TEST_ASSERT_EQUAL_UINT32(1, g.rejected);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_poll_decodes_values);
    RUN_TEST(test_offline_after_n_cycles_and_back);
    RUN_TEST(test_exception_keeps_device_online);
    RUN_TEST(test_unanswered_block_fails_the_cycle);
    RUN_TEST(test_unanswered_block_skips_the_rest_of_the_cycle);
    RUN_TEST(test_device_counters);
    RUN_TEST(test_scheduling);
    RUN_TEST(test_write_setpoint_fc06);
    RUN_TEST(test_write_coil_rejects_nan_and_inf);
    RUN_TEST(test_write_forced_multi);
    RUN_TEST(test_write_multiple_per_device);
    RUN_TEST(test_offline_device_is_probed_with_backoff);
    RUN_TEST(test_never_seen_device_is_probed_from_the_start);
    RUN_TEST(test_probe_answered_but_cycle_incomplete_stays_offline);
    RUN_TEST(test_dead_devices_do_not_delay_the_healthy_one);
    RUN_TEST(test_disabled_device_is_not_polled);
    RUN_TEST(test_mb6_decode);
    RUN_TEST(test_mb6_writes);
    RUN_TEST(test_bit_field_read);
    RUN_TEST(test_bit_field_write);
    RUN_TEST(test_scan_classes);
    RUN_TEST(test_slow_blocks_stay_due_after_a_failure);
    RUN_TEST(test_only_slow_tags);
    RUN_TEST(test_write_served_between_requests);
    RUN_TEST(test_scan_rate_override);
    RUN_TEST(test_scan_rate_and_slow_scan);
    RUN_TEST(test_forwarded_requests_counted);
    return UNITY_END();
}
