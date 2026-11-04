#include "gw_io.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gw_bsp.h"
#include "gw_debounce.h"
#include "gw_dout.h"
#include "gw_localio.h"
#include "gw_port.h"
#include "gw_status.h"
#include "gw_tagdb.h"

static const char *TAG = "io";

#define I2C_TIMEOUT_MS 50
#define WDT_FEED_SAMPLES (1000000 / CONFIG_GW_DI_SAMPLE_US)   /* feed about once per second */
#define TICK_MS        100    /* the owner task runs at least this often (failsafe, recovery) */
#define REQ_WAIT_MS    500    /* longest wait of a DO command for the owner task */
#define STACK_WORDS    (4096 / sizeof(StackType_t))

static gw_debounce_t s_db;
static void (*s_on_change)(void);
static esp_timer_handle_t s_timer;
static gw_dout_t s_do;
static bool s_exp_bus;   /* I2C bus and device created: the expander itself can be retried */
static esp_task_wdt_user_handle_t s_wdt;
static uint32_t s_wdt_count;

/* ---- the local I/O task: the only writer of the TCA9554 once it runs ---- */
typedef struct {
    uint32_t id;
    uint8_t index;
    bool on;
} do_req_t;

typedef struct {
    uint32_t id;
    bool ok;
} do_rep_t;

static gw_lio_t s_lio;
static TaskHandle_t s_task;
static StaticTask_t s_task_buf;
static StackType_t s_stack[STACK_WORDS];
static QueueHandle_t s_req_q, s_rep_q;
static StaticQueue_t s_req_buf, s_rep_buf;
static uint8_t s_req_storage[2 * sizeof(do_req_t)];
static uint8_t s_rep_storage[sizeof(do_rep_t)];
static uint32_t s_req_id;   /* the requester's (single caller: the Sparkplug task) */

#if CONFIG_GW_IO_SIMULATED

/* RAM model of the DI pins and of the TCA9554 registers. Both hold pin levels, so the
 * polarity from gw_bsp is applied exactly as on the board. Like the real expander, the
 * registers survive a software reset or a panic (RTC memory that the boot does not clear)
 * and take their power-on values only at a power-up, so IO-3 "hold" can be exercised. */
#define SIM_MAGIC 0x54434139u   /* "TCA9" */
static volatile uint8_t s_sim_pins;
static RTC_NOINIT_ATTR uint8_t s_sim_reg[4];
static RTC_NOINIT_ATTR uint32_t s_sim_magic;

static int exp_write(void *ctx, uint8_t reg, uint8_t val)
{
    (void)ctx;
    s_sim_reg[reg] = val;
    return ESP_OK;
}

static int exp_read(void *ctx, uint8_t reg, uint8_t *val)
{
    (void)ctx;
    /* Output pins read back their latch; input pins float high; POLARITY inverts the input
     * register. */
    *val = reg == GW_TCA9554_REG_INPUT
               ? (uint8_t)(((s_sim_reg[GW_TCA9554_REG_OUTPUT] & ~s_sim_reg[GW_TCA9554_REG_CONFIG]) |
                            s_sim_reg[GW_TCA9554_REG_CONFIG]) ^
                           s_sim_reg[GW_TCA9554_REG_POLARITY])
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

static esp_err_t hw_init_outputs(void)
{
    bool kept = s_sim_magic == SIM_MAGIC && esp_reset_reason() != ESP_RST_POWERON;
    if (!kept) {
        static const uint8_t power_on[4] = {0xFF, 0xFF, 0x00, 0xFF};
        memcpy(s_sim_reg, power_on, sizeof(s_sim_reg));
        s_sim_magic = SIM_MAGIC;
    }
    ESP_LOGW(TAG, "SIMULATED I/O: DOs in RAM (expander registers %s: latch 0x%02x, config 0x%02x)",
             kept ? "kept across the reset" : "at their power-on values",
             s_sim_reg[GW_TCA9554_REG_OUTPUT], s_sim_reg[GW_TCA9554_REG_CONFIG]);
    return ESP_OK;
}

static esp_err_t hw_init_inputs(void)
{
    s_sim_pins = gw_active_to_pins(0, gw_bsp.di.active_low);  /* all inputs idle */
    ESP_LOGW(TAG, "SIMULATED I/O: DIs from the console shell ('di <1-8> <0|1>')");
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

static esp_err_t hw_init_inputs(void)
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
    return gpio_config(&gc);
}

static esp_err_t hw_init_outputs(void)
{
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

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint8_t read_di(void)
{
    return gw_pins_to_active(read_pins(), gw_bsp.di.active_low);
}

/* Store the read-back in gw_tagdb and tell the Sparkplug task (NDATA by exception). */
static void publish_do(void)
{
    bool from_pins;
    uint8_t on = gw_dout_read(&s_do, &from_pins);
    if (!from_pins) {
        ESP_LOGW(TAG, "DO read-back failed; reporting the latch");
    }
    gw_tagdb_set_do(on, gw_port_time_ms());
    if (s_on_change != NULL) {
        s_on_change();
    }
}

static bool write_outputs(uint8_t mask, uint8_t active)
{
    if (!s_do.ready) {
        return false;
    }
    esp_err_t err = gw_dout_write(&s_do, mask, active);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DO write (mask 0x%02x) failed: %s", mask, esp_err_to_name(err));
    }
    publish_do();
    return err == ESP_OK;
}

static void sample_cb(void *arg)
{
    (void)arg;
    /* REL-1: the esp_timer task is not a task of ours; a TWDT user entry fed from the sampling
     * callback makes a stalled esp_timer task (no DI sampling) trip the watchdog. */
    if (s_wdt != NULL && ++s_wdt_count >= WDT_FEED_SAMPLES) {
        s_wdt_count = 0;
        esp_task_wdt_reset_user(s_wdt);
    }
    if (gw_debounce_sample(&s_db, read_di()) != 0) {
        gw_tagdb_set_di(s_db.stable, gw_port_time_ms());
        s_on_change();
        xTaskNotifyGive(s_task);
    }
}

esp_err_t gw_io_outputs_boot(uint8_t hold)
{
    esp_err_t err = hw_init_outputs();
    if (err == ESP_OK) {
        s_exp_bus = true;
        const gw_dout_bus_t bus = {.write_reg = exp_write, .read_reg = exp_read, .ctx = NULL};
        err = gw_dout_init(&s_do, &bus, gw_bsp.dout.active_low, hold);
    }
    if (err != ESP_OK) {
        /* The latch may still hold outputs from before a warm reset: publish them as unknown
         * (null), never as off, until the expander answers. */
        gw_tagdb_set_do_unknown(gw_port_time_ms());
        ESP_LOGE(TAG, "TCA9554 at 0x%02x not responding (%s): DOs disabled%s",
                 gw_bsp.dout.i2c_addr, esp_err_to_name(err),
                 s_exp_bus ? ", retrying with backoff" : "");
        return err;
    }
    publish_do();
    if (hold != 0) {
        ESP_LOGI(TAG, "DO active-low mask 0x%02x: held across the restart 0x%02x of 0x%02x, the "
                      "rest off", gw_bsp.dout.active_low, gw_dout_state(&s_do), hold);
    } else {
        ESP_LOGI(TAG, "DO active-low mask 0x%02x: all outputs off", gw_bsp.dout.active_low);
    }
    return ESP_OK;
}

void gw_io_outputs_all_off(void)
{
    s_do.hold = 0;   /* a later recovery switches everything off too */
    if (s_do.ready && gw_dout_state(&s_do) != 0) {
        ESP_LOGW(TAG, "no valid configuration: held outputs 0x%02x switched off",
                 gw_dout_state(&s_do));
        write_outputs(0xFF, 0);
    }
}

/* The safe state of a restart (IO-3), also when the expander only answers later. */
static void apply_boot_state(void)
{
    uint8_t now = gw_dout_state(&s_do);
    uint8_t want = gw_lio_boot_state(&s_lio, now);
    ESP_LOGI(TAG, "DO at restart: 0x%02x (safe state on 0x%02x, off 0x%02x, hold 0x%02x)", want,
             s_lio.safe_on, s_lio.safe_off, s_do.hold);
    if (want != now) {
        write_outputs(0xFF, want);
    }
}

/* If the expander did not answer, retry it with a backoff (1 s doubling to 60 s). */
static void recover_outputs(uint32_t now)
{
    uint16_t before = s_do.attempts;
    if (gw_dout_recover(&s_do, now)) {
        ESP_LOGW(TAG, "TCA9554 answered on attempt %u: DOs available", s_do.attempts);
        publish_do();
        apply_boot_state();
    } else if (s_do.attempts != before) {
        ESP_LOGW(TAG, "TCA9554 still not responding (attempt %u); next try in %lu ms",
                 s_do.attempts, (unsigned long)s_do.retry_delay_ms);
    }
}

static bool command_do(uint8_t index, bool on)
{
    if (!s_do.ready) {
        ESP_LOGE(TAG, "DO%u: output expander not available", index + 1);
        return false;
    }
    uint8_t bit = (uint8_t)(1u << index);
    return write_outputs(bit, on ? bit : 0);
}

static void failsafe(uint32_t now)
{
    uint8_t mask, value;
    bool up = (gw_status_get() & GW_ST_MQTT_UP) != 0;
    switch (gw_lio_failsafe(&s_lio, up, now, &mask, &value)) {
    case GW_LIO_FS_TRIP:
        ESP_LOGW(TAG, "failsafe: no Sparkplug session for %lu s: DO on 0x%02x, off 0x%02x, the "
                      "rest hold", (unsigned long)(s_lio.failsafe_ms / 1000), value,
                 (uint8_t)(mask & ~value));
        gw_tagdb_set_failsafe(true);
        if (mask != 0 && !write_outputs(mask, value)) {
            ESP_LOGE(TAG, "failsafe: outputs not driven (expander not available)");
        }
        break;
    case GW_LIO_FS_CLEAR:
        ESP_LOGI(TAG, "failsafe: Sparkplug session back; the outputs keep their state until "
                      "commanded");
        gw_tagdb_set_failsafe(false);
        break;
    default:
        break;
    }
}

static void lio_task(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TICK_MS));
        esp_task_wdt_reset();
        uint32_t now = now_ms();
        if (s_exp_bus && !s_do.ready) {
            recover_outputs(now);
        }
        do_req_t r;
        while (xQueueReceive(s_req_q, &r, 0) == pdTRUE) {
            const do_rep_t rep = {.id = r.id, .ok = command_do(r.index, r.on)};
            xQueueOverwrite(s_rep_q, &rep);
        }
        failsafe(now);
    }
}

esp_err_t gw_io_start(const gw_config_t *cfg, void (*on_change)(void))
{
    s_on_change = on_change;
    s_do.hold = gw_lio_hold_mask(cfg);   /* also for a later recovery of the expander */
    gw_lio_init(&s_lio, cfg, now_ms());
    if (s_do.ready) {
        apply_boot_state();
    }
    s_req_q = xQueueCreateStatic(2, sizeof(do_req_t), s_req_storage, &s_req_buf);
    s_rep_q = xQueueCreateStatic(1, sizeof(do_rep_t), s_rep_storage, &s_rep_buf);
    s_task = xTaskCreateStatic(lio_task, "local_io", STACK_WORDS, NULL,
                               CONFIG_GW_LOCAL_IO_TASK_PRIORITY, s_stack, &s_task_buf);
    if (cfg->local_io.failsafe_s != 0) {
        ESP_LOGI(TAG, "failsafe after %lu s without a Sparkplug session: on 0x%02x, off 0x%02x, "
                      "hold 0x%02x", (unsigned long)cfg->local_io.failsafe_s, s_lio.safe_on,
                 s_lio.safe_off, gw_lio_hold_mask(cfg));
    }

    ESP_RETURN_ON_ERROR(hw_init_inputs(), TAG, "DI gpio");
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
    ESP_RETURN_ON_ERROR(esp_task_wdt_add_user("di_sample", &s_wdt), TAG, "watchdog");
    ESP_RETURN_ON_ERROR(esp_timer_create(&ta, &s_timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_timer, CONFIG_GW_DI_SAMPLE_US), TAG, "timer");
    ESP_LOGI(TAG, "DI sampled every %d us, debounce %u ms, active-low mask 0x%02x",
             CONFIG_GW_DI_SAMPLE_US, cfg->local_io.di_debounce_ms, gw_bsp.di.active_low);
    return ESP_OK;
}

bool gw_io_set_do(uint8_t index, bool on)
{
    if (s_task == NULL) {
        ESP_LOGE(TAG, "DO%u: local I/O not started", index + 1);
        return false;
    }
    const do_req_t r = {.id = ++s_req_id, .index = index, .on = on};
    TickType_t start = xTaskGetTickCount(), wait = pdMS_TO_TICKS(REQ_WAIT_MS);
    if (xQueueSend(s_req_q, &r, wait) != pdTRUE) {
        ESP_LOGE(TAG, "DO%u: local I/O task busy", index + 1);
        return false;
    }
    xTaskNotifyGive(s_task);
    do_rep_t rep;
    for (;;) {
        TickType_t spent = xTaskGetTickCount() - start;
        if (spent >= wait || xQueueReceive(s_rep_q, &rep, wait - spent) != pdTRUE) {
            ESP_LOGE(TAG, "DO%u: no answer from the local I/O task in %d ms", index + 1,
                     REQ_WAIT_MS);
            return false;
        }
        if (rep.id == r.id) {
            return rep.ok;   /* older answers (timed out) are skipped */
        }
    }
}
