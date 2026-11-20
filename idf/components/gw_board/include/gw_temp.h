/* #212: the temperature of the ESP32-S3 (its internal sensor), for the NBIRTH (Node
 * Info/Temperature), the diagnostics and GET /api/status. The QEMU build has no sensor: no
 * reading. Any task can read it; a reading serves for 1 s. */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Install and enable the sensor (once, at boot, after gw_net_start). Issue #267: turns the SAR
 *  ADC entropy source off first (gw_rng.h). An error is logged; then no reading. */
esp_err_t gw_temp_start(void);

/** The temperature of the chip in degrees C; false when there is no reading. */
bool gw_temp_read(float *c);

#ifdef __cplusplus
}
#endif
