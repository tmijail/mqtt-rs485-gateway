/*
 * REL-1 / issue #132: the core dump in the "coredump" partition, read by the running firmware
 * for the web API, the shell and the "Node Info/Core Dump" metric.
 *
 * A panic writes the dump (ESP-IDF, CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH); it stays until it is
 * erased, across restarts. The dump is a copy of the RAM at the crash: it can hold secrets in
 * clear text (the MQTT password, TLS session keys, the private key of the device certificate).
 * Every way out of the device (GET /api/coredump, 'coredump dump') is for the administrator.
 *
 * The state and the size are read once at boot (gw_coredump_init); afterwards only an erase
 * changes them. Reads and the erase are serialized: an erase during a download makes the next
 * read fail, so the download ends incomplete instead of with mixed data. Any task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "gw_cdump.h"

#ifdef __cplusplus
extern "C" {
#endif

/** At boot, before the shell, the web server and the Sparkplug task: is a dump in flash, its
 *  size, and is its checksum correct (one read of the dump when there is one). */
void gw_coredump_init(void);

/** A dump (valid or corrupt) is in flash. No flash access. */
bool gw_coredump_present(void);

/** The state, and the size in @p size (0 = unknown). No flash access. */
gw_cdump_state_t gw_coredump_state(uint32_t *size);

/** The state, the size and, for a valid dump, its summary (esp_core_dump_get_summary and the
 *  panic reason: the dump is read). ESP_ERR_NO_MEM when there is no room for the summary. */
esp_err_t gw_coredump_summary(gw_cdump_info_t *out);

/** @p len bytes of the dump from @p offset (offset + len <= size). ESP_ERR_NOT_FOUND when there
 *  is no dump (or it was erased meanwhile), ESP_ERR_INVALID_SIZE when its size is unknown. */
esp_err_t gw_coredump_read(uint32_t offset, void *buf, size_t len);

/** Erase the dump (esp_core_dump_image_erase). @p who: for the log ("REST API", "shell"). */
esp_err_t gw_coredump_erase(const char *who);

#ifdef __cplusplus
}
#endif
