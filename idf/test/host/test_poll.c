/* QA-1: poll engine against a simulated slave: decoding, offline detection
 * (MB-9), scheduling (MB-10) and DCMD writes with read-back (SPB-10, MB-4). */
#include <string.h>

#include "gw_poll.h"
#include "gw_tagdb.h"
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
void tearDown(void) {}

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
    TEST_ASSERT_EQUAL(GW_DEV_WENT_ONLINE, gw_poller_poll(&p, 0, 0));
    gw_tag_value_t voltage = vals[0];
    s.mute_fc = MB_FC_READ_INPUT_REGISTERS;   /* Voltage's block (the last one) */
    s.holding[10] = 200;                      /* Setpoint, still answered */
    s.input[0] = 0x4300;                      /* Voltage changes to 128.0, but is never read */
    s.input[1] = 0x0000;
    for (int cycle = 1; cycle <= 2; cycle++) {
        TEST_ASSERT_EQUAL(GW_DEV_NO_CHANGE, gw_poller_poll(&p, 0, (uint32_t)(1000 * cycle)));
        TEST_ASSERT_TRUE(online());
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
    TEST_ASSERT_EQUAL(GW_WRITE_BAD_VALUE, gw_poller_write(&p, 1, 4000));  /* raw 40000 > int16 */
    s.offline = true;
    TEST_ASSERT_EQUAL(GW_WRITE_BUS_ERROR, gw_poller_write(&p, 1, 1));
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

/* ---- issue #4: offline devices are probed with a backoff ---- */

static uint32_t sim_now_ms(void)
{
    return (uint32_t)(s.clock_us / 1000u);
}

/* An offline device gets one attempt per probe, at 2, 4, 8, 10, 10 ... s (poll 1 s, cap 10 s),
 * and is back online with the first probe it answers. */
static void test_offline_device_is_probed_with_backoff(void)
{
    s.offline = true;
    uint32_t now = 0;
    for (int i = 0; i < 3; i++, now += 1000) {
        gw_poller_poll(&p, 0, now);   /* never answered: 3 full cycles (1 try + 2 retries) */
    }
    TEST_ASSERT_EQUAL(9, s.requests);
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

/* An answered probe continues the cycle; if a later block stays unanswered the device
 * (here one that never was online) stays offline and keeps backing off. */
static void test_probe_answered_but_cycle_incomplete_stays_offline(void)
{
    s.offline = true;
    for (uint32_t t = 0; t < 3000; t += 1000) {
        gw_poller_poll(&p, 0, t);
    }
    s.offline = false;
    s.mute_fc = MB_FC_READ_INPUT_REGISTERS;   /* probe (coils) answered, a later block is not */
    TEST_ASSERT_EQUAL(GW_DEV_NO_CHANGE, gw_poller_poll(&p, 0, 5000));
    TEST_ASSERT_FALSE(online());
    uint32_t wait;
    TEST_ASSERT_EQUAL(-1, gw_poller_next(&p, 5000, &wait));
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
    /* the dead ones need offline_after full cycles (3 x 3 x 200 ms each) before backing off */
    uint32_t worst = run_bus(130000, 10000, &polls);
    TEST_ASSERT_EQUAL(3, gw_poller_offline_count(&p));
    /* 120 s after the warm-up: one poll per second, delayed at most by the three probes */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(119, polls);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(1000 + 3 * (200 + 20), worst);
    gw_dev_stats_t st;
    gw_tagdb_get_stats(1, &st);
    /* 9 attempts to declare it offline, then one attempt per probe (2, 4, 8, then every 10 s) */
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(9 + 16, st.bus.timeout);
    TEST_ASSERT_EQUAL_UINT32(10000, p.probe_ms[1]);
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
    RUN_TEST(test_write_forced_multi);
    RUN_TEST(test_offline_device_is_probed_with_backoff);
    RUN_TEST(test_probe_answered_but_cycle_incomplete_stays_offline);
    RUN_TEST(test_dead_devices_do_not_delay_the_healthy_one);
    return UNITY_END();
}
