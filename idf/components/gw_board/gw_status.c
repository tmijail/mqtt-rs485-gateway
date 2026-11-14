#include "gw_status.h"

#include <stdatomic.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if !CONFIG_GW_IO_SIMULATED
#include "gw_bsp.h"
#include "led_strip.h"
#endif

static const char *TAG = "led";

static atomic_uint s_flags;

void gw_status_set(uint32_t flag, bool on)
{
    if (on) {
        atomic_fetch_or(&s_flags, flag);
    } else {
        atomic_fetch_and(&s_flags, ~flag);
    }
}

uint32_t gw_status_get(void)
{
    return atomic_load(&s_flags);
}

typedef struct {
    uint8_t r, g, b;
} rgb_t;

#define OFF    ((rgb_t){0, 0, 0})
#define RED    ((rgb_t){255, 0, 0})
#define GREEN  ((rgb_t){0, 255, 0})
#define AMBER  ((rgb_t){255, 140, 0})
#define ORANGE ((rgb_t){255, 60, 0})
#define WHITE  ((rgb_t){255, 255, 255})
#define BLUE   ((rgb_t){0, 60, 255})
#define CYAN   ((rgb_t){0, 255, 200})
#define MAGENTA ((rgb_t){255, 0, 255})

enum { B_COMMISSIONING, B_CFG_ERROR, B_NO_NET, B_NO_CLOCK, B_NO_BROKER, B_MB_FAULT, B_OPERATING };

/* The patterns below the claim window, without the access point: the color and its index. */
static rgb_t base_pattern(uint32_t f, uint32_t tick, int *idx)
{
    if (f & GW_ST_COMMISSIONING) {
        *idx = B_COMMISSIONING;
        return (tick & 4) ? OFF : BLUE;
    }
    if (f & GW_ST_CFG_ERROR) {
        *idx = B_CFG_ERROR;
        return (tick & 1) ? OFF : RED;
    }
    if (!(f & GW_ST_NET_UP)) {
        *idx = B_NO_NET;
        return RED;
    }
    if (f & GW_ST_NO_CLOCK) {
        /* #160: no MQTT session before a valid clock; the operator must act (SNTP, browser) */
        *idx = B_NO_CLOCK;
        return (tick & 4) ? OFF : MAGENTA;
    }
    if (!(f & GW_ST_MQTT_UP)) {
        *idx = B_NO_BROKER;
        return (tick & 4) ? OFF : AMBER;
    }
    if (f & GW_ST_MB_FAULT) {
        *idx = B_MB_FAULT;
        return (tick & 4) ? ORANGE : GREEN;
    }
    *idx = B_OPERATING;
    return GREEN;
}

/* Pattern for the current flags; @p tick advances every 125 ms. */
static rgb_t pattern(uint32_t f, uint32_t tick, const char **name)
{
    static const char *const plain[] = {"commissioning", "config error", "no network",
                                        "waiting for a valid clock", "no broker",
                                        "modbus device offline", "operating"};
    static const char *const with_ap[] = {"commissioning, access point",
                                          "config error, access point",
                                          "no network, access point",
                                          "waiting for a valid clock, access point",
                                          "no broker, access point",
                                          "modbus device offline, access point",
                                          "operating, access point"};
    if (f & GW_ST_RESETTING) {
        *name = "factory reset";
        return WHITE;
    }
    if (f & GW_ST_BUTTON) {
        *name = "BOOT held";
        return (tick & 1) ? OFF : WHITE;
    }
    if (f & (GW_ST_CLAIM | GW_ST_AP_OPEN)) {
        /* issue #187: the open period of the access point is the claim window (and the few
         * seconds after a claim, until the change to WPA2) */
        *name = (f & GW_ST_CLAIM) ? "claim window" : "access point open";
        return (tick & 2) ? WHITE : BLUE;
    }
    int idx;
    rgb_t c = base_pattern(f, tick, &idx);
    if (!(f & GW_ST_AP)) {
        *name = plain[idx];
        return c;
    }
    /* issue #187: the access point runs (WPA2): a cyan flash every 2 s over the pattern */
    *name = with_ap[idx];
    return (tick & 15) == 0 ? CYAN : c;
}

#if CONFIG_GW_IO_SIMULATED
typedef void *led_handle_t;

static esp_err_t led_open(led_handle_t *h)
{
    *h = NULL;
    return ESP_OK;
}

static void led_show(led_handle_t h, rgb_t c)
{
    (void)h;
    (void)c;
}
#else
typedef led_strip_handle_t led_handle_t;

static esp_err_t led_open(led_handle_t *h)
{
    const led_strip_config_t sc = {
        .strip_gpio_num = gw_bsp.status_led.gpio,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = gw_bsp.status_led.order == GW_BSP_LED_ORDER_RGB
                                      ? LED_STRIP_COLOR_COMPONENT_FMT_RGB
                                      : LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rc = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    return led_strip_new_rmt_device(&sc, &rc, h);
}

static void led_show(led_handle_t h, rgb_t c)
{
    uint32_t k = CONFIG_GW_LED_BRIGHTNESS;
    led_strip_set_pixel(h, 0, c.r * k / 255, c.g * k / 255, c.b * k / 255);
    led_strip_refresh(h);
}
#endif

static void led_task(void *arg)
{
    led_handle_t h = arg;
    const char *last = NULL;
    for (uint32_t tick = 0;; tick++) {
        const char *name;
        led_show(h, pattern(atomic_load(&s_flags), tick, &name));
        if (name != last) {
            ESP_LOGI(TAG, "status: %s", name);
            last = name;
        }
        vTaskDelay(pdMS_TO_TICKS(125));
    }
}

esp_err_t gw_status_led_start(void)
{
    led_handle_t h;
    ESP_RETURN_ON_ERROR(led_open(&h), TAG, "led");
    return xTaskCreate(led_task, "status_led", 2560, h, 2, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
