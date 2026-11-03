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
    gw_poller_opts_t o = {.merge_reads = true, .max_gap = 0, .force_multi_write = force_multi};
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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_poll_decodes_values);
    RUN_TEST(test_offline_after_n_cycles_and_back);
    RUN_TEST(test_exception_keeps_device_online);
    RUN_TEST(test_scheduling);
    RUN_TEST(test_write_setpoint_fc06);
    RUN_TEST(test_write_forced_multi);
    return UNITY_END();
}
