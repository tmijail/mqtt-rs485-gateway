/*
 * Issue #67: tell an unencrypted NVS partition from an encrypted one (CFG-7) by reading its
 * raw pages, without initialising NVS (initialising one kind with the other's settings
 * discards its entries). Pure C, host-tested with images from ESP-IDF's nvs_partition_gen.py.
 *
 * NVS page (4096 B): 32 B header (state, sequence, version, CRC32: never encrypted), 32 B
 * entry state bitmap (never encrypted), 126 entries of 32 B. With NVS encryption every entry
 * is XTS-encrypted, so the CRC32 that a plain entry carries over its own bytes does not match:
 * plain entries with a valid CRC mean an unencrypted NVS.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_NVSRAW_PAGE 4096u

typedef enum {
    GW_NVSRAW_EMPTY = 0,     /* no written entry: nothing to keep */
    GW_NVSRAW_PLAIN,         /* entries readable without encryption */
    GW_NVSRAW_ENCRYPTED,     /* written entries, none readable as plain: encrypted (or garbage) */
} gw_nvsraw_kind_t;

typedef struct {
    uint32_t pages;          /* pages in use (active, full or freeing, header CRC valid) */
    uint32_t written;        /* entries marked written (first entries and data spans) */
    uint32_t plain_ok;       /* items whose plain CRC32 matches */
    uint32_t bad;            /* written entries that do not parse as plain items */
} gw_nvsraw_scan_t;

/** CRC32 as esp_rom_crc32_le() computes it (crc is inverted on entry and exit). */
uint32_t gw_nvsraw_crc32(uint32_t crc, const uint8_t *buf, size_t len);

void gw_nvsraw_scan_init(gw_nvsraw_scan_t *s);
/** One raw page of GW_NVSRAW_PAGE bytes. */
void gw_nvsraw_scan_page(gw_nvsraw_scan_t *s, const uint8_t *page);
gw_nvsraw_kind_t gw_nvsraw_verdict(const gw_nvsraw_scan_t *s);

#ifdef __cplusplus
}
#endif
