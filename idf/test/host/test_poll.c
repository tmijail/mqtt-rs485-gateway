/* QA-1: poll engine against a simulated slave: decoding, offline detection
 * (MB-9), scheduling (MB-10) and DCMD writes with read-back (SPB-10, MB-4). */
#include <math.h>
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
    "   \"bit\": 3, \"writable\": true},"
    "  {\"name\": \"B15\", \"area\": \"input\", \"address\": 6, \"type\": \"bool\", \"bit\": 15}"
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
    RUN_TEST(test_offline_device_is_probed_with_backoff);
    RUN_TEST(test_never_seen_device_is_probed_from_the_start);
    RUN_TEST(test_probe_answered_but_cycle_incomplete_stays_offline);
    RUN_TEST(test_dead_devices_do_not_delay_the_healthy_one);
    RUN_TEST(test_mb6_decode);
    RUN_TEST(test_mb6_writes);
    return UNITY_END();
}
