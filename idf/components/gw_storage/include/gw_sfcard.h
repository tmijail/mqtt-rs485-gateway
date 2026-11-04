/*
 * SPB-14: where the store & forward spill (gw_sfspill) lives. On the board, a microSD card on
 * the SDMMC peripheral in 1-bit mode (CLK GPIO48, CMD GPIO47, D0 GPIO45, gw_bsp), FAT, in
 * /sdcard/sf. In the QEMU build (CONFIG_GW_SF_SPILL_LITTLEFS) a directory of the LittleFS
 * partition, /cfg/sf, stands in for the card (QEMU has no SDMMC).
 *
 * GPIO45 is a strapping pin (VDD_SPI voltage): a card whose D0 pull-up holds it high at reset
 * selects 1.8 V for the flash and the board may not boot. Fix VDD_SPI in eFuse (factory step,
 * irreversible) or check the schematic before enabling it (README 6.5).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "gw_sfspill.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Mount the card (or the stand-in) and fill @p io. @p where names it for the log; on
 *  failure @p why says why. Boot time only. */
bool gw_sfcard_open(gw_sfspill_io_t *io, char *where, size_t where_cap, char *why,
                    size_t why_cap);

#ifdef __cplusplus
}
#endif
