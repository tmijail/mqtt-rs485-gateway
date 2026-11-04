/*
 * Issue #67: tell an unencrypted NVS partition from an encrypted one (CFG-7) by reading its
 * raw pages, without initialising NVS (initialising one kind with the other's settings
 * discards its entries). Pure C, host-tested with images from ESP-IDF's nvs_partition_gen.py.
 *
 * NVS page (4096 B): 32 B header (state, sequence, version, CRC32: never encrypted), 32 B
 * entry state bitmap (never encrypted), 126 entries of 32 B. With NVS encryption every entry
 * is XTS-encrypted, so the CRC32 that a plain entry carries over its own bytes does not match:
 * plain entries with a valid CRC mean an unencrypted NVS.
 *
 * Issue #94 adds what the reset-safe migration needs: a u8 item read from the raw pages, the
 * persistent copy of the entries and the decision taken at boot (gw_nvsmig_*).
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
    GW_NVSRAW_MIXED,         /* issue #94: plain pages and encrypted pages (see gw_nvsmig_decide) */
} gw_nvsraw_kind_t;

typedef struct {
    uint32_t pages;          /* pages in use (active, full or freeing, header CRC valid) */
    uint32_t written;        /* entries marked written (first entries and data spans) */
    uint32_t plain_ok;       /* items whose plain CRC32 matches */
    uint32_t bad;            /* written entries that do not parse as plain items */
    uint32_t plain_pages;    /* pages with written entries, mostly plain items */
    uint32_t enc_pages;      /* pages with written entries, mostly not plain (encrypted) */
} gw_nvsraw_scan_t;

/** CRC32 as esp_rom_crc32_le() computes it (crc is inverted on entry and exit). */
uint32_t gw_nvsraw_crc32(uint32_t crc, const uint8_t *buf, size_t len);

void gw_nvsraw_scan_init(gw_nvsraw_scan_t *s);
/** One raw page of GW_NVSRAW_PAGE bytes. */
void gw_nvsraw_scan_page(gw_nvsraw_scan_t *s, const uint8_t *page);
/** EMPTY without written entries; MIXED with pages of both kinds; else PLAIN or ENCRYPTED. A page
 *  counts as plain when at least half of its written entries are plain items with a valid CRC
 *  (one corrupt entry does not make a plain page encrypted, one chance CRC match among
 *  encrypted entries does not make an encrypted page plain). */
gw_nvsraw_kind_t gw_nvsraw_verdict(const gw_nvsraw_scan_t *s);

/* ---------- issue #94: one u8 item straight from the raw pages of an unencrypted NVS ----------
 * The bdSeq when the unencrypted NVS cannot be initialised (or before it is erased): every page
 * to gw_nvsraw_u8_ns() first (finds the namespace's index), then every page to
 * gw_nvsraw_u8_item(). Only plain items with a valid CRC count; of several written copies (a
 * reset in the middle of an update) the one on the page with the highest sequence number, and
 * on that page the last one, wins. */
#define GW_NVSRAW_NAME 16u   /* namespace and key size of NVS, with the NUL */

typedef struct {
    char ns[GW_NVSRAW_NAME];
    char key[GW_NVSRAW_NAME];
    int ns_index;            /* -1 until the namespace is found */
    bool found;
    uint8_t value;
    uint32_t seq;            /* of the page the value was found on */
    uint32_t entry;          /* its entry index on that page */
} gw_nvsraw_u8_t;

void gw_nvsraw_u8_init(gw_nvsraw_u8_t *f, const char *ns, const char *key);
void gw_nvsraw_u8_ns(gw_nvsraw_u8_t *f, const uint8_t *page);
void gw_nvsraw_u8_item(gw_nvsraw_u8_t *f, const uint8_t *page);

/* ---------- issue #94: the copy of the NVS entries that makes the migration reset-safe ----------
 * The entries of the unencrypted NVS are written to a persistent copy (a LittleFS file) before
 * the partition is erased, and that copy is deleted only after the encrypted NVS was written
 * and read back. Layout: 16-byte header (magic "GWNM", version 1, record count, body length,
 * CRC32 of the body), then the records: namespace[16], key[16], type (nvs_type_t), length
 * (16-bit little endian), data (integers little endian, 1 to 8 bytes; strings with their NUL). */
#define GW_NVSMIG_MAGIC 0x4D4E5747u
#define GW_NVSMIG_HDR 16u
#define GW_NVSMIG_REC_HDR 35u

typedef struct {
    const char *ns;          /* NUL terminated, inside the buffer */
    const char *key;
    uint8_t type;
    uint16_t len;
    const uint8_t *data;
} gw_nvsmig_rec_t;

/** Start a copy in @p buf: returns the bytes used (the header), 0 if @p cap is too small. */
size_t gw_nvsmig_begin(uint8_t *buf, size_t cap);
/** Append a record; returns the new size, 0 (copy unchanged) if it does not fit or a name is
 *  empty or too long. */
size_t gw_nvsmig_add(uint8_t *buf, size_t cap, size_t used, const char *ns, const char *key,
                     uint8_t type, const void *data, size_t len);
/** Write the header (count, length, CRC); returns @p used. */
size_t gw_nvsmig_seal(uint8_t *buf, size_t used);
/** A complete, sealed copy of exactly @p size bytes (magic, version, CRC, every record whole). */
bool gw_nvsmig_check(const uint8_t *buf, size_t size, uint32_t *count);
/** Next record after offset @p off (0: the first one); false at the end or on a broken record.
 *  Works on a copy that is not sealed yet. */
bool gw_nvsmig_next(const uint8_t *buf, size_t size, size_t *off, gw_nvsmig_rec_t *r);
bool gw_nvsmig_find(const uint8_t *buf, size_t size, const char *ns, const char *key,
                    gw_nvsmig_rec_t *r);

typedef enum {
    GW_NVSMIG_COPY_NONE = 0,     /* no copy */
    GW_NVSMIG_COPY_VALID,        /* a sealed copy that passes gw_nvsmig_check() */
    GW_NVSMIG_COPY_BAD,          /* a copy that does not: discarded */
} gw_nvsmig_copy_t;

typedef enum {
    GW_NVSMIG_OPEN_ENCRYPTED = 0,  /* the normal start of the encrypted NVS */
    GW_NVSMIG_STAY_PLAIN,          /* this boot runs on the unencrypted NVS, nothing is erased */
    GW_NVSMIG_MIGRATE,             /* copy the entries, erase, write them encrypted */
    GW_NVSMIG_RESUME,              /* a migration was interrupted: write its copy encrypted */
} gw_nvsmig_action_t;

/**
 * What an encrypted build does with the NVS at boot. @p kind: the raw pages; @p copy: the copy
 * of an interrupted migration; @p on_trial: the running image is a new OTA image not confirmed
 * yet (a rollback goes back to an image that may not read an encrypted NVS: no migration until
 * it is confirmed); @p can_copy: there is a place for the copy (LittleFS mounted).
 *  - a valid copy: RESUME (only a confirmed image starts a migration);
 *  - PLAIN or MIXED: MIGRATE, or STAY_PLAIN on trial or without a place for the copy. In a MIXED
 *    partition the plain entries are the newer ones: only a build without encryption writes
 *    them, and its initialisation erases the encrypted entries it cannot read;
 *  - EMPTY on trial: STAY_PLAIN (what the previous image would write is unencrypted);
 *  - otherwise OPEN_ENCRYPTED.
 */
gw_nvsmig_action_t gw_nvsmig_decide(gw_nvsraw_kind_t kind, gw_nvsmig_copy_t copy, bool on_trial,
                                    bool can_copy);

#ifdef __cplusplus
}
#endif
