#include "gw_bootbtn.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gw_buzzer.h"
#include "gw_status.h"

#if !CONFIG_GW_IO_SIMULATED
#include "driver/gpio.h"
#include "gw_bsp.h"
#endif

static const char *TAG = "button";

#define SAMPLE_MS   20
#define STACK_WORDS (3072 / sizeof(StackType_t))

static void (*s_on_event)(gw_btn_event_t ev);
static StaticTask_t s_task_buf;
static StackType_t s_stack[STACK_WORDS];

#if CONFIG_GW_IO_SIMULATED
static volatile bool s_sim_pressed;

void gw_bootbtn_sim_set(bool pressed)
{
    s_sim_pressed = pressed;
    ESP_LOGI(TAG, "sim: BOOT %s", pressed ? "pressed" : "released");
}

static esp_err_t hw_init(void)
{
    ESP_LOGW(TAG, "SIMULATED BOOT button: console shell 'boot <0|1>'");
    return ESP_OK;
}

static bool hw_pressed(void)
{
    return s_sim_pressed;
}
#else
static esp_err_t hw_init(void)
{
    /* GPIO0 straps the boot mode: it is only read here, after the boot */
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << gw_bsp.boot_button.gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    return gpio_config(&io);
}

static bool hw_pressed(void)
{
    return gpio_get_level(gw_bsp.boot_button.gpio) == (gw_bsp.boot_button.active_low ? 0 : 1);
}

#endif

static void button_task(void *arg)
{
    (void)arg;
    gw_button_t b;
    gw_button_init(&b, (uint32_t)(esp_timer_get_time() / 1000));
    bool warned = false;
    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t before = gw_button_held_ms(&b, now);   /* length of a press that just ended */
        gw_btn_event_t ev = gw_button_update(&b, hw_pressed(), now);
        uint32_t held = gw_button_held_ms(&b, now);
        bool long_press = held >= GW_BTN_AP_MS;
        gw_status_set(GW_ST_BUTTON, long_press);
        if (long_press && !warned) {
            ESP_LOGW(TAG, "BOOT held: factory reset at %d s; release now for the access point",
                     GW_BTN_RESET_MS / 1000);
        }
        warned = long_press;
        if (ev == GW_BTN_FACTORY_RESET) {
            ESP_LOGW(TAG, "BOOT held %d s: factory reset", GW_BTN_RESET_MS / 1000);
            gw_status_set(GW_ST_RESETTING, true);
            gw_buzzer_beep_wait(2);   /* IO-6: confirmation, before the long flash erase */
        } else if (ev == GW_BTN_COMMISSIONING) {
            ESP_LOGW(TAG, "BOOT pressed %lu ms: commissioning access point", (unsigned long)before);
            gw_buzzer_beep(1);
        }
        if (ev != GW_BTN_NONE) {
            s_on_event(ev);
        }
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
    }
}

esp_err_t gw_bootbtn_start(void (*on_event)(gw_btn_event_t ev))
{
    s_on_event = on_event;
    ESP_RETURN_ON_ERROR(hw_init(), TAG, "init");
    TaskHandle_t t = xTaskCreateStatic(button_task, "boot_button", STACK_WORDS, NULL, 2, s_stack,
                                       &s_task_buf);
    return t != NULL ? ESP_OK : ESP_FAIL;
}
