#include "gw_buzzer.h"

#include <stdatomic.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gw_buzz.h"
#include "gw_ident.h"
#include "gw_status.h"

#if !CONFIG_GW_IO_SIMULATED
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "gw_bsp.h"
#endif

static const char *TAG = "buzzer";

#define TICK_MS     20      /* while a pattern plays */
#define IDLE_MS     250     /* otherwise (alarm changes are seen within this; requests wake it) */
#define STACK_WORDS (2560 / sizeof(StackType_t))

static StaticTask_t s_task_buf;
static StackType_t s_stack[STACK_WORDS];
static atomic_uint s_request;     /* beeps requested by other tasks */
static atomic_uint s_cfg;         /* alarms | after_s << 8; bumped generation in s_cfg_gen */
static atomic_uint s_cfg_gen;
static atomic_uint s_played;      /* requests the task has finished */
static atomic_uint s_queued;      /* requests made */
static atomic_uint s_alarm;       /* gw_alarm_t sounding now (issue #75) */
static bool s_ok;
static TaskHandle_t s_task;
#if CONFIG_GW_IO_SIMULATED
#define SIM_NOTE " (simulated)"
#else
#define SIM_NOTE ""
#endif

#if CONFIG_GW_IO_SIMULATED
static esp_err_t hw_init(void)
{
    ESP_LOGW(TAG, "SIMULATED buzzer: patterns only in the log");
    return ESP_OK;
}

static void hw_tone(bool on)
{
    (void)on;
}
#else
static esp_err_t hw_init(void)
{
    if (!gw_bsp.buzzer.passive) {
        const gpio_config_t io = {.pin_bit_mask = 1ULL << gw_bsp.buzzer.gpio,
                                  .mode = GPIO_MODE_OUTPUT};
        ESP_RETURN_ON_ERROR(gpio_set_level(gw_bsp.buzzer.gpio, 0), TAG, "level");
        return gpio_config(&io);
    }
    /* passive buzzer: a square wave at CONFIG_GW_BUZZER_FREQ_HZ, 50 % duty while sounding */
    const ledc_timer_config_t lt = {.speed_mode = LEDC_LOW_SPEED_MODE,
                                    .duty_resolution = LEDC_TIMER_8_BIT,
                                    .timer_num = LEDC_TIMER_3,
                                    .freq_hz = CONFIG_GW_BUZZER_FREQ_HZ,
                                    .clk_cfg = LEDC_AUTO_CLK};
    const ledc_channel_config_t lc = {.gpio_num = gw_bsp.buzzer.gpio,
                                      .speed_mode = LEDC_LOW_SPEED_MODE,
                                      .channel = LEDC_CHANNEL_7,
                                      .timer_sel = LEDC_TIMER_3,
                                      .duty = 0};
    ESP_RETURN_ON_ERROR(ledc_timer_config(&lt), TAG, "ledc timer");
    return ledc_channel_config(&lc);
}

static void hw_tone(bool on)
{
    if (!gw_bsp.buzzer.passive) {
        gpio_set_level(gw_bsp.buzzer.gpio, on ? 1 : 0);
        return;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7, on ? 128 : 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7);
}
#endif

static void buzzer_task(void *arg)
{
    (void)arg;
    gw_buzz_t b;
    gw_buzz_init(&b, 0, 0);
    unsigned gen = 0;
    unsigned playing = 0;   /* request numbers being played */
    bool tone = false;
    bool ident = false;     /* #207 */
    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        unsigned g = atomic_load(&s_cfg_gen);
        if (g != gen) {
            gen = g;
            unsigned c = atomic_load(&s_cfg);
            gw_alarm_t was = b.alarm;
            gw_buzz_init(&b, (uint8_t)(c & 0xFFu), c >> 8);
            b.alarm = was;
        }
        unsigned req = atomic_exchange(&s_request, 0);
        if (req != 0) {
            ESP_LOGI(TAG, "%u beep(s)" SIM_NOTE, req);
            gw_buzz_request(&b, (uint8_t)req);
            playing = atomic_load(&s_queued);
        }
        uint32_t f = gw_status_get();
        gw_alarm_t before = b.alarm;
        gw_alarm_t now_alarm = gw_buzz_update(&b, (f & GW_ST_CFG_ERROR) != 0,
                                              (f & GW_ST_MQTT_UP) == 0,
                                              (f & GW_ST_MB_FAULT) != 0, now);
        atomic_store(&s_alarm, (unsigned)now_alarm);
        if (now_alarm != before) {
            if (now_alarm == GW_ALARM_NONE) {
                ESP_LOGI(TAG, "alarm '%s' over: silent", gw_alarm_name(before));
            } else {
                ESP_LOGW(TAG, "alarm '%s': %d beep(s) every %u s", gw_alarm_name(now_alarm),
                         (int)now_alarm, GW_BUZZ_PERIOD_MS / 1000);
            }
        }
        bool on = gw_buzz_tone(&b, now);
        uint32_t elapsed;
        bool ident_now = gw_status_identify_elapsed(&elapsed);
        if (ident_now != ident) {
            ident = ident_now;
            if (ident) {
                ESP_LOGI(TAG, "identify: short beeps for %u s" SIM_NOTE, GW_IDENT_MS / 1000);
            } else {
                ESP_LOGI(TAG, "identify over" SIM_NOTE);
            }
        }
        if (ident) {
            on = gw_ident_tone(elapsed);   /* #207: in place of the alarm and the requests */
        }
        if (on != tone) {
            tone = on;
            hw_tone(on);
        }
        bool idle = b.request == 0 && b.left == 0 && !b.tone;
        if (idle) {
            atomic_store(&s_played, playing);
        }
        /* idle: sleep longer (less work on a core that is otherwise quiet), unless the next
         * alarm burst is closer */
        uint32_t wait = TICK_MS;
        if (idle && !ident) {
            uint32_t to_burst = b.burst_at - now;
            wait = b.alarm != GW_ALARM_NONE && to_burst < IDLE_MS ? to_burst : IDLE_MS;
            wait = wait < TICK_MS ? TICK_MS : wait;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait));
    }
}

esp_err_t gw_buzzer_start(void)
{
    esp_err_t err = hw_init();
    if (err != ESP_OK) {
        return err;
    }
    s_task = xTaskCreateStatic(buzzer_task, "buzzer", STACK_WORDS, NULL, 2, s_stack, &s_task_buf);
    s_ok = s_task != NULL;
    return s_ok ? ESP_OK : ESP_FAIL;
}

gw_alarm_t gw_buzzer_alarm(void)
{
    return (gw_alarm_t)atomic_load(&s_alarm);
}

uint8_t gw_buzzer_alarms(void)
{
    return (uint8_t)(atomic_load(&s_cfg) & 0xFFu);
}

void gw_buzzer_configure(uint8_t alarms, uint32_t after_s)
{
    atomic_store(&s_cfg, (unsigned)alarms | (unsigned)(after_s << 8));
    atomic_fetch_add(&s_cfg_gen, 1);
    if (alarms != 0) {
        ESP_LOGI(TAG, "alarms:%s%s%s (MQTT down / device offline after %lu s)",
                 alarms & GW_ALARM_BIT(GW_ALARM_CONFIG_ERROR) ? " configuration error" : "",
                 alarms & GW_ALARM_BIT(GW_ALARM_MQTT_DOWN) ? " MQTT down" : "",
                 alarms & GW_ALARM_BIT(GW_ALARM_DEVICE_OFFLINE) ? " device offline" : "",
                 (unsigned long)after_s);
    }
}

void gw_buzzer_beep(uint8_t beeps)
{
    atomic_fetch_add(&s_queued, 1);   /* first: the task reads it after taking the request */
    atomic_store(&s_request, beeps);
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

void gw_buzzer_wake(void)
{
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

void gw_buzzer_beep_wait(uint8_t beeps)
{
    if (!s_ok) {
        return;
    }
    gw_buzzer_beep(beeps);
    unsigned mine = atomic_load(&s_queued);
    /* bounded: an alarm burst in progress (3 beeps) plus this one, plus one tick */
    for (int waited = 0; waited < (beeps + 4) * 300 + 2 * TICK_MS &&
                         (int)(atomic_load(&s_played) - mine) < 0;
         waited += TICK_MS) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}
