/*
 * Minimal platform abstraction used by the pure-logic core.
 *
 * Two implementations exist: port/gw_port_esp.c (FreeRTOS + esp_log) and
 * port/gw_port_posix.c (host unit tests). Nothing else in gw_core touches the OS.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One-time initialisation (creates the global lock). Call before any task starts. */
void gw_port_init(void);

/** Wall-clock time in milliseconds since the Unix epoch (UTC). */
uint64_t gw_port_time_ms(void);

/** Global short-section lock protecting the shared tag store (gw_tagdb). Not recursive. */
void gw_port_lock(void);
void gw_port_unlock(void);

/**
 * Issue #106: lock for every use of cJSON. cJSON's hooks (the allocator), its error pointer and
 * gw_config's allocation-failure flag are process-wide, and several tasks parse JSON (web, shell,
 * boot). Recursive: a document operation (gw_cfgdoc) holds it across the gw_config_parse it
 * calls. Held for a whole validation (tens of ms to seconds): never take it while holding
 * gw_port_lock, and never in the polling or publish path.
 */
void gw_port_json_lock(void);
void gw_port_json_unlock(void);

/** Issue #267: the SAR ADC entropy source of the chip on or off (bootloader_random_enable() and
 *  bootloader_random_disable()). Only gw_rng calls it, and only to change the state. */
void gw_port_rng_sar(bool on);

#ifdef ESP_PLATFORM
#include "esp_log.h"
#define GW_LOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#define GW_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define GW_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define GW_LOGD(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)
#else
void gw_port_log(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
/** Host tests: append every log line ("<level> (<tag>) <text>\n") to @p buf; NULL stops. */
void gw_port_log_capture(char *buf, size_t cap);
/** Host tests: gw_port_time_ms() returns @p ms from now on (deterministic timestamps);
 * 0 goes back to the real clock. */
void gw_port_fake_time(uint64_t ms);
/** Host tests: the state that gw_port_rng_sar() gave last, and the number of its calls. */
bool gw_port_rng_sar_state(unsigned *calls);
#define GW_LOGE(tag, fmt, ...) gw_port_log('E', tag, fmt, ##__VA_ARGS__)
#define GW_LOGW(tag, fmt, ...) gw_port_log('W', tag, fmt, ##__VA_ARGS__)
#define GW_LOGI(tag, fmt, ...) gw_port_log('I', tag, fmt, ##__VA_ARGS__)
#define GW_LOGD(tag, fmt, ...) gw_port_log('D', tag, fmt, ##__VA_ARGS__)
#endif

#ifdef __cplusplus
}
#endif
