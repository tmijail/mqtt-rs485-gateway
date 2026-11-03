#include "gw_io.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "gw_bsp.h"
#include "gw_debounce.h"
#include "gw_dout.h"
#include "gw_port.h"
#include "gw_tagdb.h"

static const char *TAG = "io";

#define I2C_TIMEOUT_MS 50

static gw_debounce_t s_db;
static void (*s_on_change)(void);
static esp_timer_handle_t s_timer;
static gw_dout_t s_do;

#if CONFIG_GW_IO_SIMULATED

/* RAM model of the DI pins and of the TCA9554 registers (power-on values). Both
 * hold pin levels, so the polarity from gw_bsp is applied exactly as on the board. */
static volatile uint8_t s_sim_pins;
static uint8_t s_sim_reg[4] = {0xFF, 0xFF, 0x00, 0xFF};

static int exp_write(void *ctx, uint8_t reg, uint8_t val)
{
    (void)ctx;
    s_sim_reg[reg] = val;
    return ESP_OK;
}

static int exp_read(void *ctx, uint8_t reg, uint8_t *val)
{
    (void)ctx;
    /* Output pins read back their latch; input pins float high. */
    *val = reg == GW_TCA9554_REG_INPUT
               ? (uint8_t)((s_sim_reg[GW_TCA9554_REG_OUTPUT] & ~s_sim_reg[GW_TCA9554_REG_CONFIG]) |
                           s_sim_reg[GW_TCA9554_REG_CONFIG])
               : s_sim_reg[reg];
    return ESP_OK;
}

static uint8_t read_pins(void)
{
    return s_sim_pins;
}

void gw_io_sim_set_di(uint8_t index, bool active)
{
    /* The console sets the logical input; the pin takes the board's level for it. */
    uint8_t bit = (uint8_t)(1u << index);
    uint8_t now = gw_pins_to_active(s_sim_pins, gw_bsp.di.active_low);
    now = active ? (uint8_t)(now | bit) : (uint8_t)(now & ~bit);
    s_sim_pins = gw_active_to_pins(now, gw_bsp.di.active_low);
    ESP_LOGI(TAG, "sim: DI%u <- %u", index + 1, active ? 1 : 0);
}

static esp_err_t hw_init(void)
{
    s_sim_pins = gw_active_to_pins(0, gw_bsp.di.active_low);  /* all inputs idle */
    ESP_LOGW(TAG, "SIMULATED I/O: DIs from the console shell ('di <1-8> <0|1>'), DOs in RAM");
    return ESP_OK;
}

#else /* real board */

static i2c_master_dev_handle_t s_exp;

static int exp_write(void *ctx, uint8_t reg, uint8_t val)
{
    (void)ctx;
    const uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_exp, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static int exp_read(void *ctx, uint8_t reg, uint8_t *val)
{
    (void)ctx;
    return i2c_master_transmit_receive(s_exp, &reg, 1, val, 1, I2C_TIMEOUT_MS);
}

static uint8_t read_pins(void)
{
    uint8_t v = 0;
    for (int i = 0; i < GW_BSP_IO_CHANNELS; i++) {
        v |= (uint8_t)(gpio_get_level(gw_bsp.di.gpio[i]) << i);
    }
    return v;
}

static esp_err_t hw_init(void)
{
    gpio_config_t gc = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = gw_bsp.di.pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    for (int i = 0; i < GW_BSP_IO_CHANNELS; i++) {
        gc.pin_bit_mask |= 1ull << gw_bsp.di.gpio[i];
    }
    ESP_RETURN_ON_ERROR(gpio_config(&gc), TAG, "DI gpio");

    const i2c_master_bus_config_t bc = {
        .i2c_port = gw_bsp.i2c.port,
        .sda_io_num = gw_bsp.i2c.sda,
        .scl_io_num = gw_bsp.i2c.scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bc, &bus), TAG, "i2c bus");
    const i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = gw_bsp.dout.i2c_addr,
        .scl_speed_hz = gw_bsp.i2c.clk_hz,
    };
    return i2c_master_bus_add_device(bus, &dc, &s_exp);
}

#endif

static uint8_t read_di(void)
{
    return gw_pins_to_active(read_pins(), gw_bsp.di.active_low);
}

static void publish_do(void)
{
    bool from_pins;
    uint8_t on = gw_dout_read(&s_do, &from_pins);
    if (!from_pins) {
        ESP_LOGW(TAG, "DO read-back failed; reporting the latch");
    }
    gw_tagdb_set_do(on, gw_port_time_ms());
}

static void sample_cb(void *arg)
{
    (void)arg;
    if (gw_debounce_sample(&s_db, read_di()) != 0) {
        gw_tagdb_set_di(s_db.stable, gw_port_time_ms());
        s_on_change();
    }
}

esp_err_t gw_io_init(const gw_config_t *cfg, void (*on_change)(void))
{
    s_on_change = on_change;
    ESP_RETURN_ON_ERROR(hw_init(), TAG, "hw init");

    const gw_dout_bus_t bus = {.write_reg = exp_write, .read_reg = exp_read, .ctx = NULL};
    esp_err_t do_err = gw_dout_init(&s_do, &bus, gw_bsp.dout.active_low);
    if (do_err == ESP_OK) {
        publish_do();
    } else {
        ESP_LOGE(TAG, "TCA9554 at 0x%02x not responding (%s): DOs disabled", gw_bsp.dout.i2c_addr,
                 esp_err_to_name(do_err));
    }

    uint32_t threshold = (uint32_t)cfg->local_io.di_debounce_ms * 1000u / CONFIG_GW_DI_SAMPLE_US;
    uint8_t initial = read_di();
    gw_debounce_init(&s_db, (uint16_t)threshold, initial);
    gw_tagdb_set_di(initial, gw_port_time_ms());

    const esp_timer_create_args_t ta = {
        .callback = sample_cb,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "di_sample",
        .skip_unhandled_events = true,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&ta, &s_timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_timer, CONFIG_GW_DI_SAMPLE_US), TAG, "timer");
    ESP_LOGI(TAG, "DI sampled every %d us, debounce %u ms, active-low mask 0x%02x; DO active-low "
             "mask 0x%02x%s", CONFIG_GW_DI_SAMPLE_US, cfg->local_io.di_debounce_ms,
             gw_bsp.di.active_low, gw_bsp.dout.active_low, s_do.ready ? ", all off" : "");
    return do_err;
}

bool gw_io_set_do(uint8_t index, bool on)
{
    if (!s_do.ready) {
        ESP_LOGE(TAG, "DO%u: output expander not available", index + 1);
        return false;
    }
    esp_err_t err = gw_dout_set(&s_do, index, on);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DO%u write failed: %s", index + 1, esp_err_to_name(err));
    }
    publish_do();
    return err == ESP_OK;
}
