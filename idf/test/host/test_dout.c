/* QA-1 / IO-2: TCA9554 power-up sequence and DO/DI polarity, against a register
 * model of the expander that records every bus write. */
#include <string.h>

#include "gw_dout.h"
#include "unity.h"

#define ERR_BUS (-1)

typedef struct {
    uint8_t reg[4];          /* INPUT (unused: computed), OUTPUT, POLARITY, CONFIG */
    struct {
        uint8_t reg, val;
    } log[16];
    int writes;
    int fail_write_at;       /* 1-based index of the write that fails (0 = none) */
    bool absent;             /* every write fails (expander not answering) */
    bool fail_read;
} fake_tca_t;

static fake_tca_t f;

static int fake_write(void *ctx, uint8_t reg, uint8_t val)
{
    fake_tca_t *t = ctx;
    if (++t->writes == t->fail_write_at || t->absent) {
        return ERR_BUS;
    }
    t->log[t->writes - 1].reg = reg;
    t->log[t->writes - 1].val = val;
    t->reg[reg] = val;
    return 0;
}

static int fake_read(void *ctx, uint8_t reg, uint8_t *val)
{
    fake_tca_t *t = ctx;
    if (t->fail_read) {
        return ERR_BUS;
    }
    /* Output pins follow the latch; input pins float high; the input register is the pin
     * level XOR the polarity-inversion register. */
    *val = reg == GW_TCA9554_REG_INPUT
               ? (uint8_t)(((t->reg[GW_TCA9554_REG_OUTPUT] & ~t->reg[GW_TCA9554_REG_CONFIG]) |
                            t->reg[GW_TCA9554_REG_CONFIG]) ^
                           t->reg[GW_TCA9554_REG_POLARITY])
               : t->reg[reg];
    return 0;
}

static const gw_dout_bus_t k_bus = {.write_reg = fake_write, .read_reg = fake_read, .ctx = &f};

/* Pin level of the expander pin of DO @p n (1-8) as seen by the load. */
static int pin(int n)
{
    uint8_t v = (uint8_t)((f.reg[GW_TCA9554_REG_OUTPUT] & ~f.reg[GW_TCA9554_REG_CONFIG]) |
                          f.reg[GW_TCA9554_REG_CONFIG]);
    return (v >> (n - 1)) & 1;
}

void setUp(void)
{
    memset(&f, 0, sizeof(f));
    f.reg[GW_TCA9554_REG_OUTPUT] = 0xFF;   /* power-on reset values */
    f.reg[GW_TCA9554_REG_CONFIG] = 0xFF;
}
void tearDown(void) {}

static void test_polarity_mapping(void)
{
    TEST_ASSERT_EQUAL_HEX8(0x00, gw_pins_to_active(0xFF, 0xFF));  /* idle active-low group */
    TEST_ASSERT_EQUAL_HEX8(0x05, gw_pins_to_active(0xFA, 0xFF));
    TEST_ASSERT_EQUAL_HEX8(0xFA, gw_active_to_pins(0x05, 0xFF));
    TEST_ASSERT_EQUAL_HEX8(0x05, gw_pins_to_active(0x05, 0x00));  /* active high: identity */
    TEST_ASSERT_EQUAL_HEX8(0xFF, gw_pins_to_active(0xF0, 0x0F));  /* mixed: all asserted */
    for (unsigned v = 0; v < 256; v++) {
        TEST_ASSERT_EQUAL_HEX8(v, gw_pins_to_active(gw_active_to_pins((uint8_t)v, 0x3C), 0x3C));
    }
}

static void test_init_active_low_loads_off_level_before_outputs(void)
{
    gw_dout_t d;
    TEST_ASSERT_EQUAL(0, gw_dout_init(&d, &k_bus, 0xFF));
    TEST_ASSERT_TRUE(d.ready);
    /* Exactly three writes, latch first, outputs last: the pins never become outputs at an
     * ON level, and the polarity inversion is cleared in between. */
    TEST_ASSERT_EQUAL(3, f.writes);
    TEST_ASSERT_EQUAL_HEX8(GW_TCA9554_REG_OUTPUT, f.log[0].reg);
    TEST_ASSERT_EQUAL_HEX8(0xFF, f.log[0].val);
    TEST_ASSERT_EQUAL_HEX8(GW_TCA9554_REG_POLARITY, f.log[1].reg);
    TEST_ASSERT_EQUAL_HEX8(0x00, f.log[1].val);
    TEST_ASSERT_EQUAL_HEX8(GW_TCA9554_REG_CONFIG, f.log[2].reg);
    TEST_ASSERT_EQUAL_HEX8(0x00, f.log[2].val);
    for (int n = 1; n <= 8; n++) {
        TEST_ASSERT_EQUAL(1, pin(n));  /* HIGH = off for an active-low output */
    }
    bool from_pins;
    TEST_ASSERT_EQUAL_HEX8(0x00, gw_dout_read(&d, &from_pins));
    TEST_ASSERT_TRUE(from_pins);
}

static void test_warm_restart_switches_outputs_off(void)
{
    /* Pins already outputs with DO2 and DO7 energised by the previous run. */
    f.reg[GW_TCA9554_REG_CONFIG] = 0x00;
    f.reg[GW_TCA9554_REG_OUTPUT] = (uint8_t)~0x42;
    gw_dout_t d;
    TEST_ASSERT_EQUAL(0, gw_dout_init(&d, &k_bus, 0xFF));
    bool from_pins;
    TEST_ASSERT_EQUAL_HEX8(0x00, gw_dout_read(&d, &from_pins));
    TEST_ASSERT_EQUAL_HEX8(0xFF, f.reg[GW_TCA9554_REG_OUTPUT]);
}

static void test_set_active_low_drives_pin_low_and_reads_back(void)
{
    gw_dout_t d;
    gw_dout_init(&d, &k_bus, 0xFF);
    TEST_ASSERT_EQUAL(0, gw_dout_set(&d, 2, true));     /* NCMD DO/3 = true */
    TEST_ASSERT_EQUAL(0, pin(3));
    TEST_ASSERT_EQUAL_HEX8(0xFB, f.reg[GW_TCA9554_REG_OUTPUT]);
    bool from_pins;
    TEST_ASSERT_EQUAL_HEX8(0x04, gw_dout_read(&d, &from_pins));
    TEST_ASSERT_EQUAL(0, gw_dout_set(&d, 7, true));
    TEST_ASSERT_EQUAL_HEX8(0x84, gw_dout_read(&d, &from_pins));
    TEST_ASSERT_EQUAL(0, gw_dout_set(&d, 2, false));
    TEST_ASSERT_EQUAL(1, pin(3));
    TEST_ASSERT_EQUAL_HEX8(0x80, gw_dout_read(&d, &from_pins));
    TEST_ASSERT_EQUAL_HEX8(0x7F, f.reg[GW_TCA9554_REG_OUTPUT]);
}

static void test_active_high_and_mixed_polarity(void)
{
    gw_dout_t d;
    gw_dout_init(&d, &k_bus, 0x00);
    TEST_ASSERT_EQUAL_HEX8(0x00, f.log[0].val);           /* OFF = LOW when active high */
    gw_dout_set(&d, 0, true);
    TEST_ASSERT_EQUAL(1, pin(1));

    setUp();
    gw_dout_init(&d, &k_bus, 0x0F);                         /* DO1-4 active low, DO5-8 high */
    TEST_ASSERT_EQUAL_HEX8(0x0F, f.log[0].val);
    gw_dout_set(&d, 0, true);
    gw_dout_set(&d, 4, true);
    TEST_ASSERT_EQUAL(0, pin(1));
    TEST_ASSERT_EQUAL(1, pin(5));
    bool from_pins;
    TEST_ASSERT_EQUAL_HEX8(0x11, gw_dout_read(&d, &from_pins));
}

static void test_bus_errors(void)
{
    gw_dout_t d;
    f.fail_write_at = 1;                                    /* latch write fails */
    TEST_ASSERT_EQUAL(ERR_BUS, gw_dout_init(&d, &k_bus, 0xFF));
    TEST_ASSERT_FALSE(d.ready);
    TEST_ASSERT_EQUAL(1, f.writes);                         /* pins were not made outputs */
    TEST_ASSERT_EQUAL_HEX8(0xFF, f.reg[GW_TCA9554_REG_CONFIG]);

    setUp();
    TEST_ASSERT_EQUAL(0, gw_dout_init(&d, &k_bus, 0xFF));
    gw_dout_set(&d, 1, true);
    f.fail_write_at = f.writes + 1;
    TEST_ASSERT_EQUAL(ERR_BUS, gw_dout_set(&d, 5, true));  /* latch keeps the last good state */
    f.fail_read = true;
    bool from_pins;
    TEST_ASSERT_EQUAL_HEX8(0x02, gw_dout_read(&d, &from_pins));
    TEST_ASSERT_FALSE(from_pins);
}

/* Issue #8: another firmware left the polarity inversion set and the expander kept it across
 * the ESP32 reset; without clearing it the read-back would come out inverted. */
static void test_init_clears_polarity_inversion(void)
{
    f.reg[GW_TCA9554_REG_POLARITY] = 0xFF;
    gw_dout_t d;
    TEST_ASSERT_EQUAL(0, gw_dout_init(&d, &k_bus, 0xFF));
    TEST_ASSERT_EQUAL_HEX8(0x00, f.reg[GW_TCA9554_REG_POLARITY]);
    bool from_pins;
    TEST_ASSERT_EQUAL_HEX8(0x00, gw_dout_read(&d, &from_pins));   /* all off, not all on */
    gw_dout_set(&d, 2, true);
    TEST_ASSERT_EQUAL_HEX8(0x04, gw_dout_read(&d, &from_pins));
}

/* Issue #8: an expander that does not answer at boot is retried at 1, 2, 4 ... 60 s. */
static void test_recovery_with_backoff(void)
{
    gw_dout_t d;
    f.absent = true;
    TEST_ASSERT_EQUAL(ERR_BUS, gw_dout_init(&d, &k_bus, 0xFF));
    TEST_ASSERT_FALSE(d.ready);
    uint32_t now = 5000;
    TEST_ASSERT_FALSE(gw_dout_recover(&d, now));      /* schedules the first retry */
    TEST_ASSERT_EQUAL(1, d.attempts);
    const uint32_t delays[] = {1000, 2000, 4000, 8000, 16000, 32000, 60000, 60000};
    for (size_t i = 0; i < sizeof(delays) / sizeof(delays[0]); i++) {
        TEST_ASSERT_FALSE(gw_dout_recover(&d, now + delays[i] - 1));   /* not due yet */
        TEST_ASSERT_EQUAL(i + 1, d.attempts);
        now += delays[i];
        TEST_ASSERT_FALSE(gw_dout_recover(&d, now));                   /* due, fails */
        TEST_ASSERT_EQUAL(i + 2, d.attempts);
    }
    /* it answers again: the next retry loads the latch OFF, clears POLARITY, makes outputs */
    f.absent = false;
    f.writes = 0;
    f.reg[GW_TCA9554_REG_CONFIG] = 0x00;                 /* warm: outputs, DO1 left on */
    f.reg[GW_TCA9554_REG_OUTPUT] = 0xFE;
    TEST_ASSERT_TRUE(gw_dout_recover(&d, now + 60000));
    TEST_ASSERT_TRUE(d.ready);
    TEST_ASSERT_EQUAL(3, f.writes);
    TEST_ASSERT_EQUAL_HEX8(0xFF, f.reg[GW_TCA9554_REG_OUTPUT]);
    bool from_pins;
    TEST_ASSERT_EQUAL_HEX8(0x00, gw_dout_read(&d, &from_pins));
    TEST_ASSERT_FALSE(gw_dout_recover(&d, now + 120000));   /* ready: nothing to do */
    TEST_ASSERT_EQUAL(3, f.writes);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_polarity_mapping);
    RUN_TEST(test_init_active_low_loads_off_level_before_outputs);
    RUN_TEST(test_warm_restart_switches_outputs_off);
    RUN_TEST(test_set_active_low_drives_pin_low_and_reads_back);
    RUN_TEST(test_active_high_and_mixed_polarity);
    RUN_TEST(test_bus_errors);
    RUN_TEST(test_init_clears_polarity_inversion);
    RUN_TEST(test_recovery_with_backoff);
    return UNITY_END();
}
