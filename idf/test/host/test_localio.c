/* QA-1: policy of the local outputs (IO-3 safe state, IO-7 rules, #210 pulse DOs), of the pulse
 * counters (IO-4) and of the run hours (#217). */
#include <string.h>

#include "gw_config.h"
#include "gw_iocmd.h"
#include "gw_localio.h"
#include "unity.h"

static gw_config_t cfg;

void setUp(void)
{
    memset(&cfg, 0, sizeof(cfg));
}
void tearDown(void) {}

static void modes(const char *m)   /* one letter per DO: o off, 1 on, h hold */
{
    for (int i = 0; i < GW_LOCAL_IO_CHANNELS; i++) {
        cfg.local_io.do_failsafe[i] = m[i] == '1' ? GW_DO_SAFE_ON
                                      : m[i] == 'h' ? GW_DO_SAFE_HOLD : GW_DO_SAFE_OFF;
    }
}

static void test_boot_state_per_output(void)
{
    modes("o1hho1oh");
    gw_lio_t l;
    gw_lio_init(&l, &cfg, 0);
    TEST_ASSERT_EQUAL_HEX8(0x8C, gw_lio_hold_mask(&cfg));
    TEST_ASSERT_EQUAL_HEX8(0x22, l.safe_on);
    TEST_ASSERT_EQUAL_HEX8(0x51, l.safe_off);
    /* the expander kept DO3 and DO8 on (held); DO1 came back on from nowhere: off */
    TEST_ASSERT_EQUAL_HEX8(0xA6, gw_lio_boot_state(&l, 0x85));
    TEST_ASSERT_EQUAL_HEX8(0x22, gw_lio_boot_state(&l, 0x00));   /* power-up: nothing held */
}

static void test_defaults_off_and_no_failsafe(void)
{
    gw_lio_t l;
    gw_lio_init(&l, &cfg, 0);
    TEST_ASSERT_EQUAL_HEX8(0x00, gw_lio_hold_mask(&cfg));
    TEST_ASSERT_EQUAL_HEX8(0x00, gw_lio_boot_state(&l, 0xFF));   /* MVP: everything off */
    uint8_t mask, value;
    for (uint32_t t = 0; t < 10u * 86400000u; t += 3600000u) {
        TEST_ASSERT_EQUAL(GW_LIO_FS_NONE, gw_lio_failsafe(&l, false, t, &mask, &value));
    }
}

static void test_failsafe_after_n_seconds_without_session(void)
{
    modes("1ohhhhhh");
    cfg.local_io.failsafe_s = 30;
    gw_lio_t l;
    uint8_t mask, value;
    gw_lio_init(&l, &cfg, 1000);
    /* from the boot: no session yet */
    TEST_ASSERT_EQUAL(GW_LIO_FS_NONE, gw_lio_failsafe(&l, false, 30999, &mask, &value));
    TEST_ASSERT_EQUAL(GW_LIO_FS_TRIP, gw_lio_failsafe(&l, false, 31000, &mask, &value));
    TEST_ASSERT_EQUAL_HEX8(0x03, mask);
    TEST_ASSERT_EQUAL_HEX8(0x01, value);
    TEST_ASSERT_EQUAL(GW_LIO_FS_NONE, gw_lio_failsafe(&l, false, 90000, &mask, &value)); /* once */
    TEST_ASSERT_EQUAL(GW_LIO_FS_CLEAR, gw_lio_failsafe(&l, true, 95000, &mask, &value));
    TEST_ASSERT_EQUAL(GW_LIO_FS_NONE, gw_lio_failsafe(&l, true, 96000, &mask, &value));
    /* session lost at 200 s: a short outage does not trip, a long one does */
    TEST_ASSERT_EQUAL(GW_LIO_FS_NONE, gw_lio_failsafe(&l, false, 200000, &mask, &value));
    TEST_ASSERT_EQUAL(GW_LIO_FS_NONE, gw_lio_failsafe(&l, false, 229000, &mask, &value));
    TEST_ASSERT_EQUAL(GW_LIO_FS_NONE, gw_lio_failsafe(&l, true, 229500, &mask, &value));
    TEST_ASSERT_EQUAL(GW_LIO_FS_NONE, gw_lio_failsafe(&l, false, 240000, &mask, &value));
    TEST_ASSERT_EQUAL(GW_LIO_FS_NONE, gw_lio_failsafe(&l, false, 269999, &mask, &value));
    TEST_ASSERT_EQUAL(GW_LIO_FS_TRIP, gw_lio_failsafe(&l, false, 270000, &mask, &value));
}

static void test_failsafe_all_hold_and_clock_wrap(void)
{
    modes("hhhhhhhh");
    cfg.local_io.failsafe_s = 5;
    gw_lio_t l;
    uint8_t mask = 1, value = 1;
    gw_lio_init(&l, &cfg, UINT32_MAX - 1000u);   /* the 32-bit ms clock wraps meanwhile */
    TEST_ASSERT_EQUAL(GW_LIO_FS_NONE, gw_lio_failsafe(&l, false, 3000, &mask, &value));
    TEST_ASSERT_EQUAL(GW_LIO_FS_TRIP, gw_lio_failsafe(&l, false, 4000, &mask, &value));
    TEST_ASSERT_EQUAL_HEX8(0, mask);              /* tripped, but nothing to drive */
    TEST_ASSERT_EQUAL_HEX8(0, value);
}

/* IO-4: the counters are saved every counter_save_s, only when they changed */
static void test_counter_save_schedule(void)
{
    cfg.local_io.counter_save_s = 600;
    gw_lio_save_t sv;
    uint64_t c[GW_LIO_SAVE_WORDS] = {3};
    gw_lio_save_init(&sv, &cfg, c, 1000);
    TEST_ASSERT_FALSE(gw_lio_save_due(&sv, c, 1000 + 700000, false));   /* unchanged */
    TEST_ASSERT_FALSE(gw_lio_save_due(&sv, c, 1000, true));
    c[0] = 4;
    TEST_ASSERT_FALSE(gw_lio_save_due(&sv, c, 1000 + 599999, false));
    TEST_ASSERT_TRUE(gw_lio_save_due(&sv, c, 1000 + 599999, true));     /* orderly restart */
    TEST_ASSERT_TRUE(gw_lio_save_due(&sv, c, 1000 + 600000, false));
    gw_lio_saved(&sv, c, 601000);
    TEST_ASSERT_FALSE(gw_lio_save_due(&sv, c, 2000000, false));
    c[7] = 1;
    TEST_ASSERT_FALSE(gw_lio_save_due(&sv, c, 1200999, false));
    TEST_ASSERT_TRUE(gw_lio_save_due(&sv, c, 1201000, false));
    gw_lio_saved(&sv, c, 1201000);
    c[GW_LIO_SAVE_WORDS - 1] = 7;   /* #217: the run hours of DO/8 are in the same blob */
    TEST_ASSERT_FALSE(gw_lio_save_due(&sv, c, 1800999, false));
    TEST_ASSERT_TRUE(gw_lio_save_due(&sv, c, 1801000, false));

    cfg.local_io.counter_save_s = 0;                                    /* never */
    gw_lio_save_init(&sv, &cfg, (const uint64_t[GW_LIO_SAVE_WORDS]){0}, 0);
    TEST_ASSERT_FALSE(gw_lio_save_due(&sv, c, 4000000000u, false));
    TEST_ASSERT_FALSE(gw_lio_save_due(&sv, c, 1, true));
}

static void rule(uint8_t di, uint8_t dout, gw_rule_mode_t mode)
{
    uint8_t i = cfg.local_io.rule_count++;
    cfg.local_io.rules[i].di = (uint8_t)(di - 1);
    cfg.local_io.rules[i].dout = (uint8_t)(dout - 1);
    cfg.local_io.rules[i].mode = (uint8_t)mode;
}

/* IO-7: follow / invert are levels, toggle counts activations (none lost between two
 * evaluations: an even number of them changes nothing) */
static void test_rules(void)
{
    rule(2, 5, GW_RULE_FOLLOW);
    rule(2, 6, GW_RULE_INVERT);
    rule(3, 1, GW_RULE_TOGGLE);
    gw_lio_t l;
    gw_lio_init(&l, &cfg, 0);
    uint64_t c[8] = {0, 0, 7};               /* DI3 restored with 7 activations */
    gw_lio_rules_start(&l, c);
    uint8_t mask, value;
    gw_lio_rules(&l, 0x00, c, 0x00, &mask, &value);   /* boot: DI2 idle */
    TEST_ASSERT_EQUAL_HEX8(0x20, mask);               /* DO6 on (invert), the restored 7 ignored */
    TEST_ASSERT_EQUAL_HEX8(0x20, value);
    uint8_t dos = 0x20 | 0x80;                        /* DO8 is someone else's: untouched */
    gw_lio_rules(&l, 0x02, c, dos, &mask, &value);    /* DI2 active */
    TEST_ASSERT_EQUAL_HEX8(0x30, mask);
    TEST_ASSERT_EQUAL_HEX8(0x90, value & 0xB0);
    dos = (uint8_t)((dos & ~mask) | (value & mask));
    TEST_ASSERT_EQUAL_HEX8(0x90, dos);
    gw_lio_rules(&l, 0x02, c, dos, &mask, &value);
    TEST_ASSERT_EQUAL_HEX8(0x00, mask);               /* nothing changed */
    c[2] = 8;                                         /* one activation of DI3 */
    gw_lio_rules(&l, 0x06, c, dos, &mask, &value);
    TEST_ASSERT_EQUAL_HEX8(0x01, mask);
    TEST_ASSERT_EQUAL_HEX8(0x01, value & 0x01);
    dos |= 0x01;
    c[2] = 10;                                        /* two more before the next look */
    gw_lio_rules(&l, 0x02, c, dos, &mask, &value);
    TEST_ASSERT_EQUAL_HEX8(0x00, mask);
    c[2] = 13;
    gw_lio_rules(&l, 0x02, c, dos, &mask, &value);
    TEST_ASSERT_EQUAL_HEX8(0x01, mask);
    TEST_ASSERT_EQUAL_HEX8(0x00, value & 0x01);
    TEST_ASSERT_EQUAL(0, gw_lio_rule_of(&l, 4));
    TEST_ASSERT_EQUAL(2, gw_lio_rule_of(&l, 0));
    TEST_ASSERT_EQUAL(-1, gw_lio_rule_of(&l, 7));
}

/* IO-7 vs IO-3: the MQTT failsafe leaves the rule outputs alone; the restart state not */
static void test_failsafe_skips_rule_outputs(void)
{
    modes("1hoooooo");
    rule(1, 1, GW_RULE_FOLLOW);   /* would be "on" for the restart: config validation forbids it */
    rule(4, 3, GW_RULE_TOGGLE);
    cfg.local_io.failsafe_s = 1;
    gw_lio_t l;
    gw_lio_init(&l, &cfg, 0);
    uint8_t mask, value;
    TEST_ASSERT_EQUAL(GW_LIO_FS_TRIP, gw_lio_failsafe(&l, false, 1000, &mask, &value));
    TEST_ASSERT_EQUAL_HEX8(0xF8, mask);               /* DO4-8 off; DO1, DO3 rules, DO2 holds */
    TEST_ASSERT_EQUAL_HEX8(0x00, value);
    TEST_ASSERT_EQUAL_HEX8(0x00, gw_lio_boot_state(&l, 0x00) & 0x04);
}

/* #210: a pulse DO goes off after its time; a new "on" starts the time again, "off" ends it */
static void test_pulse_do(void)
{
    modes("hohooooo");
    cfg.local_io.do_pulse_ms[0] = 500;   /* DO/1: "hold" and a pulse */
    cfg.local_io.do_pulse_ms[3] = 50;
    cfg.local_io.failsafe_s = 10;
    gw_lio_t l;
    gw_lio_init(&l, &cfg, 0);
    TEST_ASSERT_EQUAL_HEX8(0x04, gw_lio_hold_mask(&cfg));        /* a pulse is not held */
    TEST_ASSERT_EQUAL_HEX8(0x04, gw_lio_boot_state(&l, 0x05));   /* nor kept at a restart */
    TEST_ASSERT_EQUAL(250, gw_lio_pulse_wait(&l, 1000, 250));     /* no pulse */
    gw_lio_pulse_cmd(&l, 0, true, 1000);
    gw_lio_pulse_cmd(&l, 2, true, 1000);                          /* DO/3: a normal DO */
    TEST_ASSERT_EQUAL_HEX8(0x01, l.pulsing);
    TEST_ASSERT_EQUAL(100, gw_lio_pulse_wait(&l, 1400, 250));
    TEST_ASSERT_EQUAL_HEX8(0x00, gw_lio_pulse_due(&l, 1499));
    gw_lio_pulse_cmd(&l, 0, true, 1300);                          /* again: until 1800 */
    TEST_ASSERT_EQUAL_HEX8(0x00, gw_lio_pulse_due(&l, 1500));
    TEST_ASSERT_EQUAL_HEX8(0x01, gw_lio_pulse_due(&l, 1800));
    TEST_ASSERT_EQUAL_HEX8(0x00, gw_lio_pulse_due(&l, 1900));    /* once */
    gw_lio_pulse_cmd(&l, 3, true, UINT32_MAX - 10u);              /* the clock wraps */
    TEST_ASSERT_EQUAL_HEX8(0x00, gw_lio_pulse_due(&l, UINT32_MAX));
    TEST_ASSERT_EQUAL_HEX8(0x08, gw_lio_pulse_due(&l, 40));
    gw_lio_pulse_cmd(&l, 3, true, 0);
    gw_lio_pulse_cmd(&l, 3, false, 10);                           /* "off" ends it */
    TEST_ASSERT_EQUAL_HEX8(0x00, gw_lio_pulse_due(&l, 100));
    /* the safe state "off" of an MQTT loss ends a pulse; "hold" lets it run */
    gw_lio_pulse_cmd(&l, 0, true, 9800);
    gw_lio_pulse_cmd(&l, 3, true, 9990);
    uint8_t mask, value;
    TEST_ASSERT_EQUAL(GW_LIO_FS_TRIP, gw_lio_failsafe(&l, false, 10000, &mask, &value));
    TEST_ASSERT_EQUAL_HEX8(0xFA, mask);
    TEST_ASSERT_EQUAL_HEX8(0x01, l.pulsing);
}

/* #217: the time on of the selected channels, given out at a change of state and every 60 s
 * while on; the other channels keep their restored value */
static void test_run_hours(void)
{
    cfg.local_io.run_hours = 0x0104;   /* DI/3 and DO/1 */
    uint64_t restored[GW_RUN_HOURS_CHANNELS] = {0};
    restored[2] = 3600;
    restored[5] = 77;                  /* DI/6: not selected */
    uint64_t out[GW_RUN_HOURS_CHANNELS];
    gw_lio_rh_t r;
    gw_lio_rh_init(&r, &cfg, restored, 0x0004, 1000);   /* DI/3 on at the boot */
    TEST_ASSERT_FALSE(gw_lio_rh_tick(&r, 0x0004, 30000, out));
    TEST_ASSERT_FALSE(gw_lio_rh_tick(&r, 0x0024, 60999, out));   /* 59.999 s: not yet */
    TEST_ASSERT_TRUE(gw_lio_rh_tick(&r, 0x0024, 61000, out));    /* a whole minute */
    TEST_ASSERT_EQUAL_UINT64(3660, out[2]);
    TEST_ASSERT_EQUAL_UINT64(77, out[5]);
    /* DI/3 goes off 2.5 s later: the exact value at once */
    TEST_ASSERT_TRUE(gw_lio_rh_tick(&r, 0x0000, 63500, out));
    TEST_ASSERT_EQUAL_UINT64(3662, out[2]);
    TEST_ASSERT_FALSE(gw_lio_rh_tick(&r, 0x0000, 500000, out));  /* off: no time */
    /* DO/1 on for 0.4 s: no whole second, nothing to give out; then on for 1.7 s more */
    TEST_ASSERT_FALSE(gw_lio_rh_tick(&r, 0x0100, 501000, out));
    TEST_ASSERT_FALSE(gw_lio_rh_tick(&r, 0x0000, 501400, out));
    TEST_ASSERT_FALSE(gw_lio_rh_tick(&r, 0x0100, 502000, out));
    TEST_ASSERT_TRUE(gw_lio_rh_tick(&r, 0x0000, 503700, out));
    TEST_ASSERT_EQUAL_UINT64(2, out[8]);
    gw_lio_rh_seconds(&r, out);
    TEST_ASSERT_EQUAL_UINT64(3662, out[2]);
    TEST_ASSERT_EQUAL_UINT64(2, out[8]);   /* 2.1 s */
    TEST_ASSERT_EQUAL_UINT64(77, out[5]);
}

/* #204: the body of POST /api/do */
static void test_web_do_request(void)
{
    uint8_t idx = 99;
    bool on = false;
    char why[96];
    const char *ok = "{\"do\": 3, \"on\": true}";
    TEST_ASSERT_TRUE(gw_iocmd_parse(ok, strlen(ok), &idx, &on, why, sizeof(why)));
    TEST_ASSERT_EQUAL(2, idx);
    TEST_ASSERT_TRUE(on);
    const char *off = "{\"on\": false, \"do\": 8}";
    TEST_ASSERT_TRUE(gw_iocmd_parse(off, strlen(off), &idx, &on, why, sizeof(why)));
    TEST_ASSERT_EQUAL(7, idx);
    TEST_ASSERT_FALSE(on);
    static const struct {
        const char *body, *why;
    } bad[] = {
        {"{\"do\": 9, \"on\": true}", "do: expected the number of the output, 1 to 8"},
        {"{\"do\": 0, \"on\": true}", "do: expected"},
        {"{\"do\": 1.5, \"on\": true}", "do: expected"},
        {"{\"do\": \"1\", \"on\": true}", "do: expected"},
        {"{\"do\": 1, \"on\": 1}", "on: expected true or false"},
        {"{\"do\": 1}", "on: expected true or false"},
        {"{\"do\": 1, \"on\": true, \"ms\": 5}", "ms: unknown key"},
        {"[1]", "expected a JSON object"},
        {"{\"do\": 1,", "not valid JSON"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE(gw_iocmd_parse(bad[i].body, strlen(bad[i].body), &idx, &on, why,
                                         sizeof(why)));
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(why, bad[i].why), why);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_boot_state_per_output);
    RUN_TEST(test_defaults_off_and_no_failsafe);
    RUN_TEST(test_failsafe_after_n_seconds_without_session);
    RUN_TEST(test_failsafe_all_hold_and_clock_wrap);
    RUN_TEST(test_counter_save_schedule);
    RUN_TEST(test_rules);
    RUN_TEST(test_failsafe_skips_rule_outputs);
    RUN_TEST(test_pulse_do);
    RUN_TEST(test_run_hours);
    RUN_TEST(test_web_do_request);
    return UNITY_END();
}
