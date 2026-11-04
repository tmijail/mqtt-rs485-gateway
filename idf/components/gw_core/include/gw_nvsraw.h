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
    uint32_t bad;            /* written entries that do not parse as plain items (a damaged string
                              * or blob that still reads as one counts once, issue #107) */
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
 * and read back. Layout: 20-byte header (magic "GWNM", version 2, record count, body length,
 * CRC32 of the body, entries the migration could not copy, reserved), then the records:
 * namespace[16], key[16], type (nvs_type_t), length (16-bit little endian), data (integers
 * little endian, 1 to 8 bytes; strings with their NUL). */
#define GW_NVSMIG_MAGIC 0x4D4E5747u
#define GW_NVSMIG_HDR 20u
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
/** Write the header (count, length, CRC, @p skipped); returns @p used. */
size_t gw_nvsmig_seal(uint8_t *buf, size_t used, uint16_t skipped);
/** A complete, sealed copy of exactly @p size bytes (magic, version, CRC, every record whole). */
bool gw_nvsmig_check(const uint8_t *buf, size_t size, uint32_t *count);
/** The CRC32 of a sealed copy's body: what identifies it (the "migration done" marker). */
uint32_t gw_nvsmig_crc(const uint8_t *buf);
/** Entries the migration that wrote the sealed copy could not copy. */
uint16_t gw_nvsmig_skipped(const uint8_t *buf);
/** Next record after offset @p off (0: the first one); false at the end or on a broken record.
 *  Works on a copy that is not sealed yet. */
bool gw_nvsmig_next(const uint8_t *buf, size_t size, size_t *off, gw_nvsmig_rec_t *r);
bool gw_nvsmig_find(const uint8_t *buf, size_t size, const char *ns, const char *key,
                    gw_nvsmig_rec_t *r);

/**
 * Issue #94: the later of two bdSeq values (0..255, wrapping), by serial-number arithmetic: @p b
 * is later when it is 1 to 127 steps after @p a; exactly 128 apart, the larger (so the result
 * does not depend on the order). Only for values with no live source between them (the RAM bdSeq
 * of a boot without NVS): a value read from the live partition always wins.
 */
uint8_t gw_nvsmig_bdseq_later(uint8_t a, uint8_t b);

/* Issue #104: a boot without NVS hands out bdSeq values from RAM. It first reserves a block of
 * them in a small LittleFS file (the first value not handed out yet); the next boot with NVS
 * continues from the reservation, so the bdSeq never goes back to a value the hosts already saw.
 * Issue #108 B3: the file also holds the base the reservations started from (the copy's or the
 * raw bdSeq). While that base is the one known, the reservation wins at any distance: serial
 * arithmetic alone picks the base again once boots without NVS have reserved 128 or more. */
#define GW_NVSMIG_BDSEQ_BLOCK 16u

typedef struct {
    bool have;               /* a reservation file was read */
    bool base_known;         /* it has its base (a one-byte file of #104 does not) */
    uint8_t base;            /* the bdSeq the chain of reservations started from */
    uint8_t next;            /* the first value not handed out yet */
} gw_nvsmig_side_t;

/** True when @p next (the next value to hand out) reached the reservation @p reserved: a new
 *  block must be reserved before it is used. */
bool gw_nvsmig_bdseq_reached(uint8_t next, uint8_t reserved);

/** The first value of a boot without NVS, from the latest bdSeq known otherwise (@p have_known,
 *  @p known: the copy's, the raw one) and the reservation; @p base: the base of the reservation
 *  this boot writes. False when nothing is known at all (the caller draws a random value). */
bool gw_nvsmig_bdseq_start(bool have_known, uint8_t known, const gw_nvsmig_side_t *side,
                           uint8_t *start, uint8_t *base);

/** The bdSeq of the first boot with NVS after boots without it: the NVS's own (@p have_nvs,
 *  @p nvs), or the reservation when it continues from that same value, else the later of both. */
uint8_t gw_nvsmig_bdseq_fold(bool have_nvs, uint8_t nvs, const gw_nvsmig_side_t *side);

/* What the read of the unencrypted NVS achieved before a migration may erase it. */
typedef struct {
    bool init_ok;            /* nvs_flash_init_partition() succeeded */
    bool init_layout_error;  /* it failed with NO_FREE_PAGES or NEW_VERSION_FOUND: IDF would erase
                              * this partition anyway, nothing can be read from it */
    bool listed_all;         /* the entry iteration ran to its end (ESP_ERR_NVS_NOT_FOUND) */
    unsigned skipped;        /* entries listed but not copied (no memory, read error, no room) */
} gw_nvsmig_read_t;

/** Issue #94: true only when every entry is in the copy (or the partition is unreadable by
 *  layout, as IDF itself would erase it). Anything else (no memory, a flash read error, an
 *  iteration that stopped early, one skipped entry) erases nothing: the boot stays unencrypted. */
bool gw_nvsmig_may_erase(const gw_nvsmig_read_t *r);

typedef enum {
    GW_NVSMIG_COPY_NONE = 0,     /* no copy */
    GW_NVSMIG_COPY_VALID,        /* read whole, and it passes gw_nvsmig_check() */
    GW_NVSMIG_COPY_BAD,          /* read whole, and it does not: the only case it is deleted */
    GW_NVSMIG_COPY_UNREADABLE,   /* it exists but could not be read whole (no memory, open or read
                                  * error): kept, it may be the only copy of the entries */
} gw_nvsmig_copy_t;

/**
 * Issue #94: a copy file read whole (@p size bytes in @p buf of @p cap) into a valid copy of the
 * current version (@p out_size bytes). VALID; BAD when it is not a copy (no magic) or fails its
 * own version's check (the only case to delete it); UNREADABLE for a version this firmware does
 * not know (kept). Version 1 (16-byte header, written by ebc2aa4) is converted in place (needs 4
 * more bytes of @p cap) and @p legacy set: its "done" marker was a u8 1, not the CRC.
 */
gw_nvsmig_copy_t gw_nvsmig_load(uint8_t *buf, size_t size, size_t cap, size_t *out_size,
                                bool *legacy, uint32_t *count);

typedef enum {
    GW_NVSMIG_OPEN_ENCRYPTED = 0,  /* the normal start of the encrypted NVS */
    GW_NVSMIG_STAY_PLAIN,          /* this boot runs on the unencrypted NVS, nothing is erased */
    GW_NVSMIG_MIGRATE,             /* copy the entries (with those of an earlier copy that the
                                    * partition no longer has), erase, write them encrypted */
    GW_NVSMIG_RESUME,              /* the partition was erased: write the copy encrypted */
    GW_NVSMIG_NO_NVS,              /* no NVS this boot: nothing is erased, opened or initialised */
} gw_nvsmig_action_t;

typedef struct {
    gw_nvsraw_kind_t kind;   /* the raw pages that could be read */
    bool raw_ok;             /* every raw page was read */
    gw_nvsmig_copy_t copy;   /* the copy of an earlier, unfinished migration */
    bool on_trial;           /* the running image is a new OTA image not confirmed yet */
    bool can_copy;           /* there is a place for the copy (LittleFS mounted) */
    /* issue #104: LittleFS does not mount but its partition is not blank, so a copy may be in it
     * (copy is then NONE: nothing could be looked at) */
    bool copy_unknown;
    bool keys;               /* the NVS keys can be read (an encrypted NVS was used here before) */
    bool mig_busy;           /* the encrypted NVS has a migration's start marker, not its end */
} gw_nvsmig_state_t;

/**
 * What an encrypted build does with the NVS at boot (issue #94):
 *  - a copy that exists but cannot be read: NO_NVS (it may hold the only copy of the entries);
 *  - issue #104: LittleFS not mountable (a copy may be there, copy_unknown) and the partition
 *    looks like an interrupted migration: EMPTY with the NVS keys present (cut right after the
 *    erase), PLAIN or MIXED with the keys present (cut during the erase, issue #108 B2), or
 *    ENCRYPTED with the start marker and not the end one (cut while writing): NO_NVS, not an
 *    empty, partly erased or partial NVS whose new entries the resume would then drop;
 *  - a raw page that cannot be read: no migration. With a valid copy NO_NVS; readable pages
 *    all encrypted OPEN_ENCRYPTED (staying unencrypted would make NVS discard their entries);
 *    readable pages all plain STAY_PLAIN (an unread page holds plain or encrypted entries; the
 *    plain ones are the newer either way, as in MIXED); nothing readable, or both kinds, NO_NVS
 *    (never a plain initialisation over a partition that may be encrypted);
 *  - PLAIN or MIXED on trial or without a place for the copy: STAY_PLAIN (a rollback must find
 *    the NVS as the previous image left it); with a valid copy NO_NVS instead (the partition is
 *    partly erased: running on it would regenerate values that a later merge keeps over the
 *    copy's; the copy is kept for the boot that migrates); else MIGRATE. A valid copy here means the erase had not finished: the partition's entries
 *    are taken first (they are the newer ones) and the copy fills in what it no longer has. In
 *    a MIXED partition the plain entries are the newer ones: only a build without encryption
 *    writes them, and its initialisation erases the encrypted entries it cannot read;
 *  - EMPTY or ENCRYPTED with a valid copy: RESUME (the erase was done, the copy is all there is);
 *  - EMPTY on trial: STAY_PLAIN (what the previous image would write is unencrypted);
 *  - otherwise OPEN_ENCRYPTED.
 */
gw_nvsmig_action_t gw_nvsmig_decide(const gw_nvsmig_state_t *s);

#ifdef __cplusplus
}
#endif
