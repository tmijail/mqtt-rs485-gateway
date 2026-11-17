#include "gw_io.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rtc_time.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#if !CONFIG_GW_IO_SIMULATED
#include "driver/pulse_cnt.h"
#endif
#include "gw_bsp.h"
#include "gw_clock.h"
#include "gw_debounce.h"
#include "gw_dout.h"
#include "gw_localio.h"
#include "gw_port.h"
#include "gw_rtc.h"
#include "gw_status.h"
#include "gw_storage.h"
#include "gw_tagdb.h"

static const char *TAG = "io";

#define I2C_TIMEOUT_MS 50
#define WDT_FEED_SAMPLES (1000000 / CONFIG_GW_DI_SAMPLE_US)   /* feed about once per second */
#define TICK_MS        250    /* the owner task runs at least this often (failsafe, recovery,
                               * counters); DI changes and DO commands wake it at once */
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
    uint8_t caller;          /* gw_io_caller_t: its answer queue */
} do_req_t;

typedef struct {
    uint32_t id;
    uint8_t result;          /* gw_io_do_result_t */
} do_rep_t;

static gw_lio_t s_lio;
static TaskHandle_t s_task;
static bool s_recovery;   /* issue #74: task started without a configuration */
static StaticTask_t s_task_buf;
static StackType_t s_stack[STACK_WORDS];
/* #204, #216: one answer queue per caller (the Sparkplug task, the web task and the Modbus TCP
 * task), so that a command of one never takes the answer of another; all use the same request
 * queue and the same checks (a DO of a local rule is refused for each) */
static QueueHandle_t s_req_q, s_rep_q[GW_IO_CALLERS];
static StaticQueue_t s_req_buf, s_rep_buf[GW_IO_CALLERS];
static uint8_t s_req_storage[2 * GW_IO_CALLERS * sizeof(do_req_t)];
static uint8_t s_rep_storage[GW_IO_CALLERS][sizeof(do_rep_t)];
static uint32_t s_req_id[GW_IO_CALLERS];   /* each caller's own (one task each) */
static const char *const k_rule_modes[] = {"follow", "invert", "toggle"};   /* gw_rule_mode_t */
static const char *const k_caller_cmd[GW_IO_CALLERS] = {"NCMD", "web command",
                                                         "Modbus TCP write"};

/* ---- #218 fast counters (PCNT) and #217 run hours ---- */
static uint8_t s_fast;                   /* DIs counted by the PCNT */
static uint8_t s_invert;                 /* #211: local_io.di[].invert */
static uint32_t s_fast_last[GW_LOCAL_IO_CHANNELS];   /* last count read */
static gw_lio_rh_t s_rh;

/* ---- TIME-2 PCF85063 RTC, on the expander's I2C bus (the local_io task owns both) ---- */
static gw_io_rtc_info_t s_rtc;
static atomic_bool s_rtc_store_req;
static uint32_t s_rtc_err_logged;

/* ---- IO-4 pulse counters and #217 run hours in NVS: one blob of GW_LIO_SAVE_WORDS ---- */
#define NVS_COUNTERS "di_count"
static gw_lio_save_t s_save;
static SemaphoreHandle_t s_save_lock;   /* the local_io task and the restart */
static StaticSemaphore_t s_save_lock_buf;

#if CONFIG_GW_IO_SIMULATED

/* RAM model of the DI pins and of the TCA9554 registers. Both hold pin levels, so the
 * polarity from gw_bsp is applied exactly as on the board. Like the real expander, the
 * registers survive a software reset or a panic (RTC memory that the boot does not clear)
 * and take their power-on values only at a power-up, so IO-3 "hold" can be exercised. */
#define SIM_MAGIC 0x54434139u   /* "TCA9" */
static volatile uint8_t s_sim_pins;
static RTC_NOINIT_ATTR uint8_t s_sim_reg[4];
static RTC_NOINIT_ATTR uint32_t s_sim_magic;

/* Issue #74: 'expander absent <ms>' makes the simulated TCA9554 silent for the first <ms> of the
 * next boot only (kept in RTC memory across the restart). */
#define SIM_ABSENT_MAGIC 0x41425354u   /* "ABST" */
static RTC_NOINIT_ATTR uint32_t s_sim_absent_magic;
static RTC_NOINIT_ATTR uint32_t s_sim_absent_next_ms;
static uint32_t s_sim_absent_ms;   /* this boot */

void gw_io_sim_expander_absent(uint32_t ms)
{
    s_sim_absent_next_ms = ms;
    s_sim_absent_magic = SIM_ABSENT_MAGIC;
    ESP_LOGW(TAG, "sim: the output expander will not answer for %lu ms after the next boot",
             (unsigned long)ms);
}

static int64_t s_sim_absent_from_us;   /* first expander access of this boot */

static bool sim_absent(void)
{
    if (s_sim_absent_ms == 0) {
        return false;
    }
    /* measured from this boot's first access: QEMU's timer is not reset by a software reset */
    int64_t now = esp_timer_get_time();
    if (s_sim_absent_from_us == 0) {
        s_sim_absent_from_us = now;
    }
    return now - s_sim_absent_from_us < (int64_t)s_sim_absent_ms * 1000;
}

static int exp_write(void *ctx, uint8_t reg, uint8_t val)
{
    (void)ctx;
    if (sim_absent()) {
        return ESP_ERR_TIMEOUT;
    }
    s_sim_reg[reg] = val;
    return ESP_OK;
}

static int exp_read(void *ctx, uint8_t reg, uint8_t *val)
{
    (void)ctx;
    if (sim_absent()) {
        return ESP_ERR_TIMEOUT;
    }
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

/* #218: the simulated PCNT units: the edges of the inputs that are fast counters */
static volatile uint32_t s_sim_pcnt[GW_LOCAL_IO_CHANNELS];

void gw_io_sim_set_di(uint8_t index, bool active)
{
    /* The console sets the level of the input with the board's polarity (before the #211
     * inversion); the pin takes the board's level for it. */
    uint8_t bit = (uint8_t)(1u << index);
    uint8_t now = gw_pins_to_active(s_sim_pins, gw_bsp.di.active_low);
    uint8_t before = now;
    now = active ? (uint8_t)(now | bit) : (uint8_t)(now & ~bit);
    s_sim_pins = gw_active_to_pins(now, gw_bsp.di.active_low);
    /* the PCNT counts the activations of the inverted level, as the board's edge action */
    if ((s_fast & bit) && ((now ^ s_invert) & bit) && !((before ^ s_invert) & bit)) {
        s_sim_pcnt[index]++;
    }
    ESP_LOGI(TAG, "sim: DI%u <- %u", index + 1, active ? 1 : 0);
}

bool gw_io_sim_pulses(uint8_t index, uint32_t n)
{
    if (index >= GW_LOCAL_IO_CHANNELS || !(s_fast & (1u << index))) {
        return false;
    }
    s_sim_pcnt[index] += n;
    ESP_LOGI(TAG, "sim: %lu pulses on the PCNT unit of DI%u", (unsigned long)n, index + 1);
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
    return true;
}

static esp_err_t fast_init(const gw_config_t *cfg)
{
    (void)cfg;
    if (s_fast != 0) {
        ESP_LOGW(TAG, "SIMULATED PCNT: fast counters 0x%02x count the 'di' edges and 'pcnt' "
                      "pulses of the console", s_fast);
    }
    return ESP_OK;
}

static uint32_t fast_read(uint8_t index)
{
    return s_sim_pcnt[index];
}

static uint32_t fast_delta(uint8_t index, uint32_t now)
{
    return now - s_fast_last[index];
}

static esp_err_t hw_init_outputs(void)
{
    bool kept = s_sim_magic == SIM_MAGIC && esp_reset_reason() != ESP_RST_POWERON;
    if (s_sim_absent_magic == SIM_ABSENT_MAGIC && esp_reset_reason() != ESP_RST_POWERON) {
        s_sim_absent_ms = s_sim_absent_next_ms;   /* this boot only */
        ESP_LOGW(TAG, "SIMULATED I/O: expander silent for the first %lu ms of this boot",
                 (unsigned long)s_sim_absent_ms);
    }
    s_sim_absent_magic = 0;
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

/* Simulated PCF85063: like the chip, it keeps counting across a software reset or a panic
 * (RTC memory the boot does not clear, plus the RTC timer of the ESP32-S3) and comes up with
 * the OS bit set at a power-up (a new QEMU process). */
#define SIM_RTC_MAGIC 0x50434638u   /* "PCF8" */
static RTC_NOINIT_ATTR uint32_t s_sim_rtc_magic;
static RTC_NOINIT_ATTR int64_t s_sim_rtc_epoch;    /* time written ... */
static RTC_NOINIT_ATTR uint64_t s_sim_rtc_us;      /* ... at this RTC timer value */

static esp_err_t hw_init_rtc(void)
{
    if (s_sim_rtc_magic != SIM_RTC_MAGIC || esp_reset_reason() == ESP_RST_POWERON) {
        s_sim_rtc_magic = 0;
    }
    ESP_LOGW(TAG, "SIMULATED RTC: PCF85063 in RTC memory (%s)",
             s_sim_rtc_magic == SIM_RTC_MAGIC ? "kept across the reset" : "power-up: OS bit set");
    return ESP_OK;
}

static int rtc_read(uint8_t regs[GW_RTC_REGS])
{
    if (s_sim_rtc_magic != SIM_RTC_MAGIC) {
        memset(regs, 0, GW_RTC_REGS);
        regs[0] = GW_RTC_OS_BIT;
        regs[3] = regs[5] = 1;
        return ESP_OK;
    }
    uint64_t now = esp_rtc_get_time_us();
    int64_t t = s_sim_rtc_epoch + (now >= s_sim_rtc_us ? (int64_t)((now - s_sim_rtc_us) / 1000000)
                                                      : 0);
    return gw_rtc_encode(t, regs) ? ESP_OK : ESP_FAIL;
}

static int rtc_write(const uint8_t regs[GW_RTC_REGS])
{
    int64_t t;
    if (gw_rtc_decode(regs, &t) != GW_RTC_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    s_sim_rtc_epoch = t;
    s_sim_rtc_us = esp_rtc_get_time_us();
    s_sim_rtc_magic = SIM_RTC_MAGIC;
    return ESP_OK;
}

#else /* real board */

static i2c_master_dev_handle_t s_exp;
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_rtc_dev;

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

/* #218: one PCNT unit per fast DI. Without the accumulation of the driver (no interrupt): the
 * counter goes back to 0 at PCNT_HIGH, and the local_io task reads it at least every 250 ms, so
 * it counts up to about 130 kHz without a loss (far above what the input circuit passes). */
#define PCNT_HIGH 32767
static pcnt_unit_handle_t s_pcnt[GW_LOCAL_IO_CHANNELS];

static esp_err_t fast_init(const gw_config_t *cfg)
{
    for (uint8_t i = 0; i < GW_LOCAL_IO_CHANNELS; i++) {
        if (!(s_fast & (1u << i))) {
            continue;
        }
        const pcnt_unit_config_t uc = {.low_limit = -1, .high_limit = PCNT_HIGH};
        ESP_RETURN_ON_ERROR(pcnt_new_unit(&uc, &s_pcnt[i]), TAG, "PCNT unit of DI%u", i + 1);
        if (cfg->local_io.glitch_ns[i] != 0) {
            const pcnt_glitch_filter_config_t fc = {.max_glitch_ns = cfg->local_io.glitch_ns[i]};
            ESP_RETURN_ON_ERROR(pcnt_unit_set_glitch_filter(s_pcnt[i], &fc), TAG,
                                "glitch filter of DI%u", i + 1);
        }
        const pcnt_chan_config_t cc = {.edge_gpio_num = gw_bsp.di.gpio[i], .level_gpio_num = -1};
        pcnt_channel_handle_t ch;
        ESP_RETURN_ON_ERROR(pcnt_new_channel(s_pcnt[i], &cc, &ch), TAG, "PCNT channel");
        /* count the activations: the edge to the active level, after the board's polarity and
         * the #211 inversion */
        bool active_high = !(((gw_bsp.di.active_low ^ s_invert) >> i) & 1u);
        ESP_RETURN_ON_ERROR(
            pcnt_channel_set_edge_action(
                ch, active_high ? PCNT_CHANNEL_EDGE_ACTION_INCREASE : PCNT_CHANNEL_EDGE_ACTION_HOLD,
                active_high ? PCNT_CHANNEL_EDGE_ACTION_HOLD : PCNT_CHANNEL_EDGE_ACTION_INCREASE),
            TAG, "PCNT edge");
        ESP_RETURN_ON_ERROR(pcnt_unit_enable(s_pcnt[i]), TAG, "PCNT enable");
        ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(s_pcnt[i]), TAG, "PCNT clear");
        ESP_RETURN_ON_ERROR(pcnt_unit_start(s_pcnt[i]), TAG, "PCNT start");
        ESP_LOGI(TAG, "DI%u counted by a PCNT unit, glitch filter %u ns", i + 1,
                 cfg->local_io.glitch_ns[i]);
    }
    return ESP_OK;
}

static uint32_t fast_read(uint8_t index)
{
    int v = 0;
    if (s_pcnt[index] == NULL || pcnt_unit_get_count(s_pcnt[index], &v) != ESP_OK || v < 0) {
        return s_fast_last[index];
    }
    return (uint32_t)v;
}

static uint32_t fast_delta(uint8_t index, uint32_t now)
{
    return (now + PCNT_HIGH - s_fast_last[index]) % PCNT_HIGH;
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
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bc, &s_bus), TAG, "i2c bus");
    const i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = gw_bsp.dout.i2c_addr,
        .scl_speed_hz = gw_bsp.i2c.clk_hz,
    };
    return i2c_master_bus_add_device(s_bus, &dc, &s_exp);
}

/* TIME-2: the PCF85063 shares the bus created for the expander (gw_bsp: SDA 42, SCL 41). */
static esp_err_t hw_init_rtc(void)
{
    if (s_bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = GW_PCF85063_ADDR,
        .scl_speed_hz = gw_bsp.i2c.clk_hz,
    };
    return i2c_master_bus_add_device(s_bus, &dc, &s_rtc_dev);
}

static int rtc_read(uint8_t regs[GW_RTC_REGS])
{
    const uint8_t reg = GW_PCF85063_REG_TIME;
    return i2c_master_transmit_receive(s_rtc_dev, &reg, 1, regs, GW_RTC_REGS, I2C_TIMEOUT_MS);
}

static int rtc_write(const uint8_t regs[GW_RTC_REGS])
{
    uint8_t buf[1 + GW_RTC_REGS] = {GW_PCF85063_REG_TIME};
    memcpy(buf + 1, regs, GW_RTC_REGS);   /* writing the seconds clears the OS bit */
    return i2c_master_transmit(s_rtc_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

#endif

/* ---------- TIME-2 RTC ---------- */

void gw_io_rtc_restore(void)
{
    esp_err_t err = hw_init_rtc();
    s_rtc.present = err == ESP_OK;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RTC: no I2C bus (%s): no time before SNTP", esp_err_to_name(err));
        return;
    }
    uint8_t regs[GW_RTC_REGS];
    err = rtc_read(regs);
    if (err != ESP_OK) {
        s_rtc.present = false;
        ESP_LOGE(TAG, "RTC: PCF85063 at 0x%02x not responding (%s): no time before SNTP",
                 GW_PCF85063_ADDR, esp_err_to_name(err));
        return;
    }
    int64_t t = 0;
    s_rtc.status = gw_rtc_decode(regs, &t);
    if (s_rtc.status != GW_RTC_OK) {
        ESP_LOGW(TAG, "RTC: %s: system time not set (until SNTP or a time from the browser)",
                 gw_rtc_status_str(s_rtc.status));
        if (s_rtc.status == GW_RTC_STOPPED) {
            /* #160: the battery keeps the time while the power is off (TIME-2) */
            s_rtc.lost = true;
            ESP_LOGW(TAG, "RTC: %s", gw_clock_hint_text(GW_CLOCK_HINT_BATTERY));
        }
        return;
    }
    const struct timeval tv = {.tv_sec = (time_t)t, .tv_usec = 0};
    settimeofday(&tv, NULL);
    s_rtc.restored = true;
    struct tm tm;
    time_t tt = (time_t)t;
    gmtime_r(&tt, &tm);
    ESP_LOGI(TAG, "RTC: system time set from the RTC: %04d-%02d-%02dT%02d:%02d:%02dZ (not "
                  "synchronised until SNTP)", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* Store the system time (just synchronised by SNTP, or set from the browser: #160) in the RTC.
 * Owner of the bus only. */
static void rtc_store_now(void)
{
    if (!s_rtc.present) {
        return;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint8_t regs[GW_RTC_REGS];
    if (!gw_rtc_encode((int64_t)tv.tv_sec, regs)) {
        ESP_LOGW(TAG, "RTC: system time %lld out of the RTC's range: not stored",
                 (long long)tv.tv_sec);
        return;
    }
    esp_err_t err = rtc_write(regs);
    if (err != ESP_OK) {
        s_rtc.store_errors++;
        if (s_rtc_err_logged++ < 3) {
            ESP_LOGE(TAG, "RTC: write failed (%s)", esp_err_to_name(err));
        }
        return;
    }
    s_rtc.stores++;
    s_rtc.status = GW_RTC_OK;
    if (s_rtc.stores == 1) {
        ESP_LOGI(TAG, "RTC: time stored in the RTC (again after every SNTP synchronisation and "
                      "every time set from the browser)");
    }
}

void gw_io_rtc_store(void)
{
    if (s_task == NULL) {
        rtc_store_now();   /* no local_io task (yet): this caller is the only bus user */
        return;
    }
    atomic_store(&s_rtc_store_req, true);
    xTaskNotifyGive(s_task);
}

void gw_io_rtc_get(gw_io_rtc_info_t *out)
{
    *out = s_rtc;
}

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
        gw_tagdb_update_di(s_db.stable, gw_port_time_ms());   /* counts the activations */
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
        ESP_LOGW(TAG, "TCA9554 answered on attempt %u: DOs available%s", s_do.attempts,
                 s_recovery ? ", all off (no valid configuration)" : "");
        publish_do();
        if (!s_recovery) {
            apply_boot_state();
        }
    } else if (s_do.attempts != before) {
        ESP_LOGW(TAG, "TCA9554 still not responding (attempt %u); next try in %lu ms",
                 s_do.attempts, (unsigned long)s_do.retry_delay_ms);
    }
}

static gw_io_do_result_t command_do(uint8_t caller, uint8_t index, bool on)
{
    int rule = gw_lio_rule_of(&s_lio, index);
    if (rule >= 0) {
        ESP_LOGW(TAG, "DO%u is driven by local rule %d (%s DI%u): %s refused", index + 1, rule,
                 k_rule_modes[s_lio.rules[rule].mode % 3], s_lio.rules[rule].di + 1,
                 k_caller_cmd[caller % GW_IO_CALLERS]);
        return GW_IO_DO_RULE;
    }
    if (!s_do.ready) {
        ESP_LOGE(TAG, "DO%u: output expander not available", index + 1);
        return GW_IO_DO_UNAVAILABLE;
    }
    uint8_t bit = (uint8_t)(1u << index);
    if (caller == GW_IO_CALLER_WEB) {
        ESP_LOGI(TAG, "DO%u %s by a web command", index + 1, on ? "on" : "off");
    }
    if (!write_outputs(bit, on ? bit : 0)) {
        return GW_IO_DO_UNAVAILABLE;
    }
    /* #210: a pulse DO goes off by itself after its time (end_pulses) */
    gw_lio_pulse_cmd(&s_lio, index, on, now_ms());
    if (on && (s_lio.pulse & bit)) {
        ESP_LOGI(TAG, "DO%u: pulse of %u ms", index + 1, s_lio.pulse_ms[index]);
    }
    return GW_IO_DO_OK;
}

/* #210: switch off the pulse DOs whose time ended */
static void end_pulses(uint32_t now)
{
    uint8_t due = gw_lio_pulse_due(&s_lio, now);
    if (due != 0 && s_do.ready) {
        ESP_LOGD(TAG, "pulse end: DO mask 0x%02x off", due);
        write_outputs(due, 0);
    }
}

/* #218: the new pulses of the PCNT units */
static void poll_fast(void)
{
    bool changed = false;
    for (uint8_t i = 0; i < GW_LOCAL_IO_CHANNELS; i++) {
        if (!(s_fast & (1u << i))) {
            continue;
        }
        uint32_t v = fast_read(i);
        uint32_t d = fast_delta(i, v);
        s_fast_last[i] = v;
        if (d != 0) {
            gw_tagdb_add_count(i, d, gw_port_time_ms());
            changed = true;
        }
    }
    if (changed && s_on_change != NULL) {
        s_on_change();
    }
}

/* #217: count the time on; a new value goes to gw_tagdb (and the next NDATA) at a change of state
 * and every GW_RUN_HOURS_REPORT_S while on */
static void run_hours(uint32_t now)
{
    if (s_rh.mask == 0) {
        return;
    }
    gw_local_io_t io;
    gw_tagdb_get_io(&io);
    uint16_t on = (uint16_t)(io.di | (io.do_unknown ? 0u : (uint16_t)io.dout << 8));
    uint64_t out[GW_RUN_HOURS_CHANNELS];
    if (xSemaphoreTake(s_save_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    bool changed = gw_lio_rh_tick(&s_rh, on, now, out);
    xSemaphoreGive(s_save_lock);
    if (changed) {
        gw_tagdb_set_run_hours(out, gw_port_time_ms());
        if (s_on_change != NULL) {
            s_on_change();
        }
    }
}

/* IO-7: apply the local rules to the current DIs (also without the network). */
static void rules(void)
{
    if (s_lio.rule_count == 0 || !s_do.ready) {
        return;
    }
    gw_local_io_t io;
    gw_tagdb_get_io(&io);
    uint8_t mask, value;
    gw_lio_rules(&s_lio, io.di, io.count, gw_dout_state(&s_do), &mask, &value);
    if (mask != 0) {
        ESP_LOGD(TAG, "rules: DO mask 0x%02x -> 0x%02x", mask, value & mask);
        write_outputs(mask, value);
    }
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

/* The buffers of save_counters, in PSRAM (allocated once in gw_io_start) */
typedef struct {
    gw_local_io_t io;
    uint64_t v[GW_LIO_SAVE_WORDS];   /* the counters, then the run hours (#217) */
} save_buf_t;
static save_buf_t *s_save_buf;

/* IO-4: save the counters if they changed and it is time (or @p force: orderly restart). */
static void save_counters(bool force)
{
    if (s_save_buf == NULL) {
        return;
    }
    if (xSemaphoreTake(s_save_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    /* not on the stack (s_save_buf, under s_save_lock): on_shutdown() runs in the task that calls
     * esp_restart(), for example the esp_timer task (restart_soon), with a small stack */
    gw_local_io_t *io = &s_save_buf->io;
    uint64_t *v = s_save_buf->v;
    gw_tagdb_get_io(io);
    uint32_t now = now_ms();
    memcpy(v, io->count, sizeof(io->count));
    gw_lio_rh_seconds(&s_rh, &v[GW_LOCAL_IO_CHANNELS]);
    if (gw_lio_save_due(&s_save, v, now, force)) {
        esp_err_t err = gw_storage_set_blob(NVS_COUNTERS, v, sizeof(s_save_buf->v));
        if (err == ESP_OK) {
            gw_lio_saved(&s_save, v, now);
            ESP_LOGI(TAG, "pulse counters saved%s", force ? " before the restart" : "");
        } else if (err != ESP_ERR_INVALID_STATE || !force) {
            ESP_LOGW(TAG, "pulse counters not saved: %s", esp_err_to_name(err));
            gw_lio_saved(&s_save, s_save.saved, now);   /* retry after one period */
        }
    }
    xSemaphoreGive(s_save_lock);
}

static void on_shutdown(void)
{
    save_counters(true);   /* esp_restart(): NCMD Reboot, new configuration, shell, web */
    /* the stack that the save left in the task of esp_restart() (the E2E reads it) */
    ESP_LOGI(TAG, "shutdown in task '%s': %u B of its stack never used", pcTaskGetName(NULL),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static void lio_task(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    for (;;) {
        /* #210: wake at the end of a pulse */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(gw_lio_pulse_wait(&s_lio, now_ms(), TICK_MS)));
        esp_task_wdt_reset();
        uint32_t now = now_ms();
        if (s_exp_bus && !s_do.ready) {
            recover_outputs(now);
        }
        if (s_recovery) {
            /* issue #74: no configuration: only the expander retry (target all off: the hold
             * mask was cleared) and the RTC */
            if (atomic_exchange(&s_rtc_store_req, false)) {
                rtc_store_now();
            }
            continue;
        }
        if (atomic_exchange(&s_rtc_store_req, false)) {
            rtc_store_now();   /* TIME-2: the bus has one user, this task */
        }
        do_req_t r;
        while (xQueueReceive(s_req_q, &r, 0) == pdTRUE) {
            const do_rep_t rep = {.id = r.id,
                                  .result = (uint8_t)command_do(r.caller, r.index, r.on)};
            xQueueOverwrite(s_rep_q[r.caller % GW_IO_CALLERS], &rep);
        }
        end_pulses(now);
        poll_fast();
        rules();
        failsafe(now);
        run_hours(now);
        save_counters(false);
    }
}

static void create_queues(void)
{
    s_req_q = xQueueCreateStatic(2 * GW_IO_CALLERS, sizeof(do_req_t), s_req_storage, &s_req_buf);
    for (int i = 0; i < GW_IO_CALLERS; i++) {
        s_rep_q[i] = xQueueCreateStatic(1, sizeof(do_rep_t), s_rep_storage[i], &s_rep_buf[i]);
    }
}

esp_err_t gw_io_start_recovery(void)
{
    s_recovery = true;
    s_do.hold = 0;   /* target: every DO off, held ones too */
    create_queues();
    s_task = xTaskCreateStatic(lio_task, "local_io", STACK_WORDS, NULL,
                               CONFIG_GW_LOCAL_IO_TASK_PRIORITY, s_stack, &s_task_buf);
    if (s_task == NULL) {
        return ESP_FAIL;
    }
    if (s_exp_bus && !s_do.ready) {
        ESP_LOGW(TAG, "no valid configuration: retrying the output expander to switch every DO off");
    }
    return ESP_OK;
}

void gw_io_get_expander(gw_io_expander_t *out)
{
    out->ready = s_do.ready;
    out->retrying = s_exp_bus && !s_do.ready && s_task != NULL;
    out->attempts = s_do.attempts;
}

esp_err_t gw_io_start(const gw_config_t *cfg, void (*on_change)(void))
{
    s_on_change = on_change;
    s_do.hold = gw_lio_hold_mask(cfg);   /* also for a later recovery of the expander */
    gw_lio_init(&s_lio, cfg, now_ms());
    if (s_do.ready) {
        apply_boot_state();
    }
    if (cfg->local_io.failsafe_s != 0) {
        ESP_LOGI(TAG, "failsafe after %lu s without a Sparkplug session: on 0x%02x, off 0x%02x, "
                      "hold 0x%02x", (unsigned long)cfg->local_io.failsafe_s, s_lio.safe_on,
                 s_lio.safe_off, gw_lio_hold_mask(cfg));
    }

    /* IO-4 / #217: counters and run hours from the last save (lost since then after a power
     * cut or a panic). Before v1.0 the blob grew from 8 to 24 values: an older one is not read
     * (the counters start at 0 once). */
    uint64_t saved[GW_LIO_SAVE_WORDS] = {0};
    bool restored = gw_storage_get_blob(NVS_COUNTERS, saved, sizeof(saved));
    const uint64_t *count = saved, *run_s = &saved[GW_LOCAL_IO_CHANNELS];
    s_fast = cfg->local_io.fast_mask;
    s_invert = cfg->local_io.di_invert;
    gw_tagdb_set_fast(s_fast);   /* #218: their debounced activations do not count */
    gw_tagdb_set_counters(count);
    gw_tagdb_set_run_hours(run_s, gw_port_time_ms());
    s_save_lock = xSemaphoreCreateMutexStatic(&s_save_lock_buf);
    s_save_buf = gw_storage_calloc(1, sizeof(*s_save_buf));
    if (s_save_buf == NULL) {
        ESP_LOGE(TAG, "pulse counters: no memory for the save: they are not saved");
    }
    gw_lio_save_init(&s_save, cfg, saved, now_ms());
    if (cfg->local_io.counter_save_s != 0) {
        esp_register_shutdown_handler(on_shutdown);
    }
    ESP_LOGI(TAG, "pulse counters %s (%" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
                  " %" PRIu64 " %" PRIu64 " %" PRIu64 "), %s %lu s",
             restored ? "restored" : "start at 0", count[0], count[1], count[2], count[3],
             count[4], count[5], count[6], count[7],
             cfg->local_io.counter_save_s != 0 ? "saved every" : "never saved: counter_save_s",
             (unsigned long)cfg->local_io.counter_save_s);

    esp_err_t in_err = hw_init_inputs();
    /* #211: a debounce and the inversion for each DI */
    uint16_t threshold[GW_LOCAL_IO_CHANNELS];
    for (int i = 0; i < GW_LOCAL_IO_CHANNELS; i++) {
        threshold[i] = gw_debounce_samples(cfg->local_io.di_debounce_ms[i], CONFIG_GW_DI_SAMPLE_US);
    }
    uint8_t raw = in_err == ESP_OK ? read_di() : 0;
    gw_debounce_init_each(&s_db, threshold, s_invert, raw);
    gw_tagdb_set_di(s_db.stable, gw_port_time_ms());
    if (in_err == ESP_OK) {
        esp_err_t fe = fast_init(cfg);
        if (fe != ESP_OK) {
            ESP_LOGE(TAG, "fast counters (PCNT) not available: %s", esp_err_to_name(fe));
        }
        for (uint8_t i = 0; i < GW_LOCAL_IO_CHANNELS; i++) {
            s_fast_last[i] = (s_fast & (1u << i)) ? fast_read(i) : 0;
        }
    }
    /* #217: the run hours count from the states of the boot */
    gw_lio_rh_init(&s_rh, cfg, run_s,
                   (uint16_t)(s_db.stable | (s_do.ready ? (uint16_t)gw_dout_state(&s_do) << 8 : 0u)),
                   now_ms());
    if (cfg->local_io.run_hours != 0) {
        ESP_LOGI(TAG, "run hours of 0x%04x (bit n DI/n+1, bit 8+n DO/n+1), %s", cfg->local_io.run_hours,
                 restored ? "restored" : "start at 0");
    }
    /* IO-7: the rules start from the boot's DIs and counters (restored pulses toggle nothing);
     * the local_io task evaluates them on its first pass */
    gw_lio_rules_start(&s_lio, count);
    for (uint8_t i = 0; i < cfg->local_io.rule_count; i++) {
        ESP_LOGI(TAG, "local rule %u: DO%u %s DI%u (NCMD on DO%u refused, no MQTT failsafe)", i,
                 cfg->local_io.rules[i].dout + 1, k_rule_modes[cfg->local_io.rules[i].mode % 3],
                 cfg->local_io.rules[i].di + 1, cfg->local_io.rules[i].dout + 1);
    }

    create_queues();
    s_task = xTaskCreateStatic(lio_task, "local_io", STACK_WORDS, NULL,
                               CONFIG_GW_LOCAL_IO_TASK_PRIORITY, s_stack, &s_task_buf);
    ESP_RETURN_ON_ERROR(in_err, TAG, "DI gpio");

    const esp_timer_create_args_t ta = {
        .callback = sample_cb,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "di_sample",
        .skip_unhandled_events = true,
    };
    ESP_RETURN_ON_ERROR(esp_task_wdt_add_user("di_sample", &s_wdt), TAG, "watchdog");
    ESP_RETURN_ON_ERROR(esp_timer_create(&ta, &s_timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_timer, CONFIG_GW_DI_SAMPLE_US), TAG, "timer");
    const uint16_t *db = cfg->local_io.di_debounce_ms;
    ESP_LOGI(TAG, "DI sampled every %d us, debounce (ms) %u %u %u %u %u %u %u %u, inverted 0x%02x, "
                  "active-low mask 0x%02x, fast 0x%02x",
             CONFIG_GW_DI_SAMPLE_US, db[0], db[1], db[2], db[3], db[4], db[5], db[6], db[7],
             s_invert, gw_bsp.di.active_low, s_fast);
    return ESP_OK;
}

gw_io_do_result_t gw_io_command_do(gw_io_caller_t caller, uint8_t index, bool on)
{
    if (s_task == NULL || s_recovery || caller >= GW_IO_CALLERS || index >= GW_LOCAL_IO_CHANNELS) {
        ESP_LOGE(TAG, "DO%u: local I/O not started", index + 1);
        return GW_IO_DO_UNAVAILABLE;
    }
    const do_req_t r = {.id = ++s_req_id[caller], .index = index, .on = on, .caller = caller};
    TickType_t start = xTaskGetTickCount(), wait = pdMS_TO_TICKS(REQ_WAIT_MS);
    if (xQueueSend(s_req_q, &r, wait) != pdTRUE) {
        ESP_LOGE(TAG, "DO%u: local I/O task busy", index + 1);
        return GW_IO_DO_BUSY;
    }
    xTaskNotifyGive(s_task);
    do_rep_t rep;
    for (;;) {
        TickType_t spent = xTaskGetTickCount() - start;
        if (spent >= wait || xQueueReceive(s_rep_q[caller], &rep, wait - spent) != pdTRUE) {
            ESP_LOGE(TAG, "DO%u: no answer from the local I/O task in %d ms", index + 1,
                     REQ_WAIT_MS);
            return GW_IO_DO_BUSY;
        }
        if (rep.id == r.id) {
            return (gw_io_do_result_t)rep.result;   /* older answers (timed out) are skipped */
        }
    }
}

bool gw_io_set_do(uint8_t index, bool on)
{
    return gw_io_command_do(GW_IO_CALLER_SPARKPLUG, index, on) == GW_IO_DO_OK;
}
