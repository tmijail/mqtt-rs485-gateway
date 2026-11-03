/* ESP-IDF implementation of gw_port.h. */
#include "gw_port.h"

#include <assert.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;

void gw_port_init(void)
{
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    assert(s_lock != NULL);
}

uint64_t gw_port_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000u + (uint64_t)(tv.tv_usec / 1000);
}

void gw_port_lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

void gw_port_unlock(void)
{
    xSemaphoreGive(s_lock);
}
