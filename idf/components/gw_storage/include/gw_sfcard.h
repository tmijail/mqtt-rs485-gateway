/*
 * SPB-14: where the store & forward spill (gw_sfspill) lives. On the board, a microSD card on
 * the SDMMC peripheral in 1-bit mode (CLK GPIO48, CMD GPIO47, D0 GPIO45, gw_bsp), FAT, in
 * /sdcard/sf. In the QEMU build (CONFIG_GW_SF_SPILL_LITTLEFS) a directory of the LittleFS
 * partition, /cfg/sf, stands in for the card (QEMU has no SDMMC).
 *
 * #124: the files are the ring's extents (r0000.sf ..), its two superblock copies (ringa.sf,
 * ringb.sf) and, until they are migrated, the segments of the earlier format (NNNNNNNN.sf), all
 * 8.3 names. They stay open between calls (one handle each for the extent being written, the
 * one being read, a delete, and each superblock copy), so a card operation allocates nothing
 * except when it opens a file. A block or a superblock is whole aligned sectors with no fsync:
 * FatFs writes them straight to the card, with no FAT or directory write. The SDMMC transfers
 * from PSRAM go through one DMA-capable buffer allocated at mount (the driver would allocate a
 * bounce buffer for each). Each operation is timed for the board tests (gw_sfcard_get_stats,
 * the shell's `status`).
 *
 * GPIO45 is a strapping pin (VDD_SPI voltage): a card whose D0 pull-up holds it high at reset
 * selects 1.8 V for the flash and the board may not boot. Check the schematic before enabling
 * it (README 6.5).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
    GW_SFCARD_MARK,     /* in-place write of the dead mark of an old segment, with its fsync */
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

/** Mount the card (or the stand-in) and fill @p io. @p where names it for the log; on
 *  failure @p why says why. Boot time only. */
bool gw_sfcard_open(gw_sfspill_io_t *io, char *where, size_t where_cap, char *why,
                    size_t why_cap);

/** Close every open file (the spill stopped: before a format). The owner of the card only. */
void gw_sfcard_close(void);

/**
 * #124: format the card as FAT (FAT32 from 2 GB; FAT16 below, where FAT32 cannot be) with
 * 32 KiB clusters. Everything on it is lost. Only after gw_sfcard_close(), with no other card
 * access. False with @p msg saying why when there is no card to format (the QEMU stand-in, no
 * card mounted); else @p msg tells the result.
 */
bool gw_sfcard_format(char *msg, size_t cap);

/** Copy of the operation times and counters (any task). */
void gw_sfcard_get_stats(gw_sfcard_stats_t *out);

/** Name of an operation kind, for the shell. */
const char *gw_sfcard_op_name(gw_sfcard_op_t op);

#ifdef __cplusplus
}
#endif
