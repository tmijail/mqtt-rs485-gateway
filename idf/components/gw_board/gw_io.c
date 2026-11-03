#include "gw_io.h"

#include <stdio.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gw_debounce.h"
#include "gw_port.h"
#include "gw_tagdb.h"

#if CONFIG_GW_IO_SIMULATED
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#endif

static const char *TAG = "io";

/* TCA9554 registers. Power-on state: output latch 0xFF, all pins inputs. */
#define TCA9554_INPUT  0x00
#define TCA9554_OUTPUT 0x01
#define TCA9554_CONFIG 0x03   /* bit = 1: input */
#define I2C_TIMEOUT_MS 50

static gw_debounce_t s_db;
static void (*s_on_change)(void);
static esp_timer_handle_t s_timer;
static uint8_t s_do_latch;
static bool s_do_ready;

#if CONFIG_GW_IO_SIMULATED

static volatile uint8_t s_sim_di;
static uint8_t s_sim_reg[4] = {0x00, 0xFF, 0x00, 0xFF};

static esp_err_t exp_write(uint8_t reg, uint8_t val)
{
    s_sim_reg[reg] = val;
    return ESP_OK;
}

static esp_err_t exp_read(uint8_t reg, uint8_t *val)
{
    *val = reg == TCA9554_INPUT ? (uint8_t)(s_sim_reg[TCA9554_OUTPUT] & ~s_sim_reg[TCA9554_CONFIG])
                                : s_sim_reg[reg];
    return ESP_OK;
}

static uint8_t read_di(void)
{
    return s_sim_di;  /* logical levels, set from the console */
}

static void sim_console_task(void *arg)
{
    (void)arg;
    char line[32];
    for (;;) {
        unsigned ch, level;
        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (sscanf(line, "di %u %u", &ch, &level) == 2 && ch >= 1 && ch <= 8) {
            uint8_t bit = (uint8_t)(1u << (ch - 1));
            s_sim_di = level ? (uint8_t)(s_sim_di | bit) : (uint8_t)(s_sim_di & ~bit);
            ESP_LOGI(TAG, "sim: DI%u <- %u", ch, level ? 1 : 0);
        }
    }
}

static esp_err_t hw_init(void)
{
    ESP_RETURN_ON_ERROR(uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, NULL, 0), TAG,
                        "console uart");
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
    ESP_LOGW(TAG, "SIMULATED I/O: DIs from the console ('di <1-8> <0|1>'), DOs in RAM");
    return xTaskCreate(sim_console_task, "sim_di", 3072, NULL, 1, NULL) == pdPASS ? ESP_OK
                                                                                  : ESP_ERR_NO_MEM;
}

#else /* real board */

static i2c_master_dev_handle_t s_exp;

static esp_err_t exp_write(uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_exp, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t exp_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_exp, &reg, 1, val, 1, I2C_TIMEOUT_MS);
}

static uint8_t read_di(void)
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint8_t)(gpio_get_level((gpio_num_t)(CONFIG_GW_DI_FIRST_GPIO + i)) << i);
    }
#if CONFIG_GW_DI_ACTIVE_LOW
    v = (uint8_t)~v;
#endif
    return v;
}

#if CONFIG_GW_DI_ACTIVE_LOW
#define DI_PULL_UP GPIO_PULLUP_ENABLE
#else
#define DI_PULL_UP GPIO_PULLUP_DISABLE
#endif

static esp_err_t hw_init(void)
{
    const gpio_config_t gc = {
        .pin_bit_mask = 0xFFull << CONFIG_GW_DI_FIRST_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = DI_PULL_UP,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&gc), TAG, "DI gpio");

    const i2c_master_bus_config_t bc = {
        .i2c_port = -1,
        .sda_io_num = CONFIG_GW_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_GW_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bc, &bus), TAG, "i2c bus");
    const i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_GW_DO_EXPANDER_ADDR,
        .scl_speed_hz = CONFIG_GW_I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(bus, &dc, &s_exp);
}

#endif

static void publish_do(void)
{
    uint8_t level = s_do_latch;
    if (exp_read(TCA9554_INPUT, &level) != ESP_OK) {
        ESP_LOGW(TAG, "DO read-back failed; reporting the latch");
    }
    gw_tagdb_set_do(level, gw_port_time_ms());
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

    /* Load the output latch while the pins are still inputs (power-on) or already
     * outputs (warm restart): either way the DOs never pulse on. */
    esp_err_t do_err = exp_write(TCA9554_OUTPUT, 0x00);
    if (do_err == ESP_OK) {
        do_err = exp_write(TCA9554_CONFIG, 0x00);
    }
    if (do_err == ESP_OK) {
        s_do_latch = 0;
        s_do_ready = true;
        publish_do();
    } else {
        ESP_LOGE(TAG, "TCA9554 at 0x%02x not responding (%s): DOs disabled",
                 CONFIG_GW_DO_EXPANDER_ADDR, esp_err_to_name(do_err));
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
    ESP_LOGI(TAG, "DI sampled every %d us, debounce %u ms%s", CONFIG_GW_DI_SAMPLE_US,
             cfg->local_io.di_debounce_ms, s_do_ready ? "; DOs off" : "");
    return do_err;
}

bool gw_io_set_do(uint8_t index, bool on)
{
    if (!s_do_ready) {
        ESP_LOGE(TAG, "DO%u: output expander not available", index + 1);
        return false;
    }
    uint8_t bit = (uint8_t)(1u << index);
    uint8_t latch = on ? (uint8_t)(s_do_latch | bit) : (uint8_t)(s_do_latch & ~bit);
    esp_err_t err = exp_write(TCA9554_OUTPUT, latch);
    if (err == ESP_OK) {
        s_do_latch = latch;
    } else {
        ESP_LOGE(TAG, "DO%u write failed: %s", index + 1, esp_err_to_name(err));
    }
    publish_do();
    return err == ESP_OK;
}
