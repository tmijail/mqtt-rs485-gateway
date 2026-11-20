#include "gw_temp.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gw_rng.h"

#if !CONFIG_GW_IO_SIMULATED
#include "driver/temperature_sensor.h"
#endif

static const char *TAG = "io";

#define CACHE_US 1000000   /* a reading serves the callers for 1 s (the web UI polls every 2 s) */

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static bool s_valid;
static float s_c;
static int64_t s_at = -CACHE_US;

#if CONFIG_GW_IO_SIMULATED
/* QEMU has no model of the temperature sensor: no reading (null in the NBIRTH, "not available" in
 * the web UI) */
esp_err_t gw_temp_start(void)
{
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    gw_rng_adc_starts("the temperature sensor starts");   /* issue #267: the same sequence */
    ESP_LOGI(TAG, "temperature sensor: none in the emulator (SIMULATED): no reading");
    return ESP_OK;
}

static bool sensor_read(float *c)
{
    (void)c;
    return false;
}
#else
static temperature_sensor_handle_t s_sensor;

esp_err_t gw_temp_start(void)
{
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    /* issue #267: the sensor uses the SAR ADC; the SAR ADC entropy source must be off first */
    gw_rng_adc_starts("the temperature sensor starts");
    /* the range of the ESP32-S3 sensor with an error below 2 degrees C that covers a panel: the
     * chip is warmer than the air around it */
    const temperature_sensor_config_t tc = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
    esp_err_t e = temperature_sensor_install(&tc, &s_sensor);
    if (e == ESP_OK) {
        e = temperature_sensor_enable(s_sensor);
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "temperature sensor unavailable (%s): no reading", esp_err_to_name(e));
        s_sensor = NULL;
    }
    return e;
}

static bool sensor_read(float *c)
{
    return s_sensor != NULL && temperature_sensor_get_celsius(s_sensor, c) == ESP_OK;
}
#endif

bool gw_temp_read(float *c)
{
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    int64_t now = esp_timer_get_time();
    if (now - s_at >= CACHE_US) {
        float v;
        s_valid = sensor_read(&v);
        s_c = s_valid ? v : 0.0f;
        s_at = now;
    }
    bool ok = s_valid;
    *c = s_c;
    xSemaphoreGive(s_lock);
    return ok;
}
