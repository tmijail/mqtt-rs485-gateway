/*
 * SPB-14: where the store & forward spill (gw_sfspill) lives. On the board, a microSD card on
 * the SDMMC peripheral in 1-bit mode (CLK GPIO48, CMD GPIO47, D0 GPIO45, gw_bsp), FAT, in
 * /sdcard/sf. In the QEMU build (CONFIG_GW_SF_SPILL_LITTLEFS) a directory of the LittleFS
 * partition, /cfg/sf, stands in for the card (QEMU has no SDMMC).
 *
 * #124: the files are the ring's extents (r0000.sf ..) and its two superblock copies (ringa.sf,
 * ringb.sf), all 8.3 names. A file with another name is never read, changed or deleted (#134).
 * The files stay open between calls (one handle each for the extent being written, the
 * one being read, a delete, and each superblock copy), so a card operation allocates nothing
 * except when it opens a file. A block or a superblock is whole aligned sectors with no fsync:
 * FatFs writes them straight to the card, with no FAT or directory write. The SDMMC transfers
 * from PSRAM go through one DMA-capable buffer allocated at mount (the driver would allocate a
 * bounce buffer for each). Each operation is timed for the board tests (gw_sfcard_get_stats,
 * the shell's `status`).
 *
 * #139: the card has one owner at a time: the spill (from the boot mount), or a format. The
 * format runs in a task of its own (created for it, deleted after it), asks the spill to give
 * the card up first, then starts the SDMMC host and the card itself: it also formats a card that
 * did not mount, or that the spill does not use. The boot never formats. The card's state and
 * the format's state are kept here for the shell and /api/status (gw_cardfmt).
 *
 * GPIO45 is a strapping pin (VDD_SPI voltage): a card whose D0 pull-up holds it high at reset
 * selects 1.8 V for the flash and the board may not boot. Check the schematic before enabling
 * it (docs/reference/store-and-forward.md#the-microsd-card-and-gpio45).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gw_cardfmt.h"
#include "gw_sfspill.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Kinds of card operation that gw_sfcard times. */
typedef enum {
    GW_SFCARD_OPEN,     /* open (and close) of a file */
    GW_SFCARD_BLOCK,    /* write of one 4 KiB block */
    GW_SFCARD_SUPER,    /* write of one superblock copy (a checkpoint or a head hint) */
    GW_SFCARD_READ,
    GW_SFCARD_ALLOC,    /* allocation of an extent or a superblock file (lseek and fsync) */
    GW_SFCARD_STEP,     /* one delete step: truncate (with its fsync) or unlink */
    GW_SFCARD_STAT,     /* size of a file that is not open */
    GW_SFCARD_LIST,     /* the directory listing of the boot */
    GW_SFCARD_OP_COUNT
} gw_sfcard_op_t;

typedef struct {
    uint32_t count;
    uint32_t max_us;
    uint64_t total_us;
} gw_sfcard_time_t;

typedef struct {
    gw_sfcard_time_t op[GW_SFCARD_OP_COUNT];
    uint32_t errors;        /* operations that failed */
    uint32_t unit;          /* allocation unit (FAT cluster) in bytes, 0 = unknown */
} gw_sfcard_stats_t;

/**
 * Mount the card (or the stand-in) for the spill and fill @p io. @p where names it for the
 * log; on failure @p why says why. Boot time only. The spill owns the card from then on:
 * when a format needs it, gw_sfcard calls @p release (from the format task); the owner must
 * then stop using the card and call gw_sfcard_close() (#139).
 */
bool gw_sfcard_open(gw_sfspill_io_t *io, char *where, size_t where_cap, char *why,
                    size_t why_cap, void (*release)(void));

/** The owner stops using the card for good (until the restart): every open file is closed
 *  and the card is free for a format. The owner of the card only. */
void gw_sfcard_close(void);

/** The boot did not look for the card (the spill did not start before the mount): @p why,
 *  for the card's state. */
void gw_sfcard_unchecked(const char *why);

/**
 * #139 / #137: start a format of the card in a task of its own, and return at once. The card
 * becomes FAT32 (FAT16 below 2 GB) with 32 KiB clusters, in one MBR partition; everything on
 * it is lost. It also formats a card that did not mount (no file system, exFAT, NTFS, ext4,
 * GPT), and a card that the spill does not use (store_forward.sd off). A spill that uses the
 * card stops first (the RAM ring goes on). Never automatic: only the shell's and the web
 * API's confirmed requests call this. STARTED, or why not: a format runs, the build has no
 * card, there is no PSRAM, no memory for the task. A card that does not answer ends the
 * format as failed (gw_sfcard_get_format). Restart the gateway to use the card afterwards.
 */
gw_cardfmt_answer_t gw_sfcard_format_start(void);

/** CONFIG_GW_IO_SIMULATED only: a format that writes nothing and takes @p ms, through the same
 *  task, the same release of the spill and the same states (the E2E). */
gw_cardfmt_answer_t gw_sfcard_format_simulate(uint32_t ms);

/** Copies of the format's state and of the card's state (any task). */
void gw_sfcard_get_format(gw_cardfmt_t *out);
void gw_sfcard_get_info(gw_card_info_t *out);

/** The format task's stack: its size and, after a format, the least free since its start (B). */
void gw_sfcard_format_stack(uint32_t *size, uint32_t *min_free);

/** Copy of the operation times and counters (any task). */
void gw_sfcard_get_stats(gw_sfcard_stats_t *out);

/** Name of an operation kind, for the shell. */
const char *gw_sfcard_op_name(gw_sfcard_op_t op);

#ifdef __cplusplus
}
#endif
