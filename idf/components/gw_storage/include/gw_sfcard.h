/*
 * SPB-14: where the store & forward spill (gw_sfspill) lives. On the board, a microSD card on
 * the SDMMC peripheral in 1-bit mode (CLK GPIO48, CMD GPIO47, D0 GPIO45, gw_bsp), FAT, in
 * /sdcard/sf. In the QEMU build (CONFIG_GW_SF_SPILL_LITTLEFS) a directory of the LittleFS
 * partition, /cfg/sf, stands in for the card (QEMU has no SDMMC).
 *
 * #122: the segment files stay open between calls (one handle for the segment being written,
 * one for reads, one for deletes), so a card operation allocates nothing except when it opens
 * a segment. Writes and reads from PSRAM go through one DMA-capable buffer allocated at mount
 * (the SDMMC driver would allocate a bounce buffer for each of them). Each operation is timed
 * for the board tests (gw_sfcard_get_stats, the shell's `status`).
 *
 * GPIO45 is a strapping pin (VDD_SPI voltage): a card whose D0 pull-up holds it high at reset
 * selects 1.8 V for the flash and the board may not boot. Fix VDD_SPI in eFuse (factory step,
 * irreversible) or check the schematic before enabling it (README 6.5).
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
    GW_SFCARD_OPEN,     /* open (and close) of a segment file */
    GW_SFCARD_APPEND,   /* write of records or of a new header */
    GW_SFCARD_SYNC,     /* fsync after a write, a dead mark or a truncate */
    GW_SFCARD_READ,
    GW_SFCARD_MARK,     /* in-place write of the dead mark */
    GW_SFCARD_STEP,     /* one delete step: truncate (with its fsync) or unlink */
    GW_SFCARD_STAT,     /* size of a segment that is not open */
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

/** Copy of the operation times and counters (any task). */
void gw_sfcard_get_stats(gw_sfcard_stats_t *out);

/** Name of an operation kind, for the shell. */
const char *gw_sfcard_op_name(gw_sfcard_op_t op);

#ifdef __cplusplus
}
#endif
