/*
 * PCF85063 real-time clock (TIME-2): register image <-> UTC time, pure logic.
 *
 * The time registers 0x04..0x0A hold, in BCD: seconds (bit 7 = OS, "oscillator stopped":
 * the clock lost power or never started, its time is not to be trusted), minutes, hours
 * (24-hour mode), day of the month, weekday (0 = Sunday), month and year (00..99 =
 * 2000..2099). gw_board reads and writes them over I2C (or a simulated chip on QEMU); this
 * module decides whether a reading is a time the gateway may use.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_PCF85063_ADDR      0x51
#define GW_PCF85063_REG_CTRL1 0x00
#define GW_PCF85063_REG_TIME  0x04   /* first of the 7 time registers */
#define GW_RTC_REGS           7
#define GW_RTC_OS_BIT         0x80   /* in the seconds register */

/* A reading before this is not plausible (the firmware is newer): 2025-01-01T00:00:00Z. */
#define GW_RTC_MIN_EPOCH 1735689600LL
/* The chip counts years 00..99 from 2000: 2099-12-31T23:59:59Z is the last time it holds. */
#define GW_RTC_MAX_EPOCH 4102444799LL

typedef enum {
    GW_RTC_OK = 0,
    GW_RTC_STOPPED,      /* OS bit set: the oscillator stopped (power lost) */
    GW_RTC_INVALID,      /* not BCD, or a field out of range (day 31 of April, hour 24...) */
    GW_RTC_IMPLAUSIBLE,  /* valid but before GW_RTC_MIN_EPOCH */
} gw_rtc_status_t;

uint8_t gw_bcd_to_bin(uint8_t bcd);
uint8_t gw_bin_to_bcd(uint8_t bin);

/** Decode the 7 time registers into seconds since the Unix epoch (UTC). */
gw_rtc_status_t gw_rtc_decode(const uint8_t regs[GW_RTC_REGS], int64_t *epoch_s);

/**
 * Registers for @p epoch_s (UTC), OS bit clear. Returns false (and writes nothing) outside
 * GW_RTC_MIN_EPOCH..GW_RTC_MAX_EPOCH: a clock that SNTP has not set is never stored.
 */
bool gw_rtc_encode(int64_t epoch_s, uint8_t regs[GW_RTC_REGS]);

/** Text of a status, for the log and the shell. */
const char *gw_rtc_status_str(gw_rtc_status_t st);

#ifdef __cplusplus
}
#endif
