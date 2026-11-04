/*
 * Digital outputs on a TCA9554 8-bit I2C expander (IO-2) and the pin polarity
 * of 8-channel I/O groups. Pure logic over an abstract register bus, so the
 * power-up sequence and the polarity handling are host-tested; gw_board
 * supplies the I2C (or simulated) bus and the active levels from gw_bsp.
 *
 * Logical state: bit n = channel n+1 active (DO energised, DI asserted).
 * Pin level: logical XOR active_low, where bit n of active_low set means
 * channel n+1 is active when its pin is LOW.
 *
 * Power-up: the TCA9554 resets with its output latch at 0xFF and every pin an
 * input. gw_dout_init() loads the latch with the OFF level of every channel
 * before it turns the pins into outputs, so the initialisation never energises
 * a DO, on a cold boot or on a warm restart (where the pins already are
 * outputs and keep their previous level until the latch is rewritten). It also
 * clears the polarity-inversion register: the expander keeps it across an
 * ESP32 reset, and a value left by other firmware would invert the read-back.
 *
 * An expander that does not answer (for example an I2C glitch at boot) is
 * retried with a backoff by gw_dout_recover().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_TCA9554_REG_INPUT  0x00   /* pin levels */
#define GW_TCA9554_REG_OUTPUT 0x01   /* output latch */
#define GW_TCA9554_REG_POLARITY 0x02 /* bit = 1: input register bit inverted */
#define GW_TCA9554_REG_CONFIG 0x03   /* bit = 1: input */

/** Logical states from pin levels (the mapping is its own inverse). */
static inline uint8_t gw_pins_to_active(uint8_t pins, uint8_t active_low)
{
    return (uint8_t)(pins ^ active_low);
}

/** Pin levels for logical states. */
static inline uint8_t gw_active_to_pins(uint8_t active, uint8_t active_low)
{
    return (uint8_t)(active ^ active_low);
}

typedef struct {
    /** Write / read one expander register; 0 on success, a driver error code otherwise. */
    int (*write_reg)(void *ctx, uint8_t reg, uint8_t val);
    int (*read_reg)(void *ctx, uint8_t reg, uint8_t *val);
    void *ctx;
} gw_dout_bus_t;

typedef struct {
    gw_dout_bus_t bus;
    uint8_t active_low;   /* bit n: DO n+1 is energised by a LOW pin */
    uint8_t latch;        /* pin levels last written to the output latch */
    bool ready;           /* false until gw_dout_init() succeeded */
    uint16_t attempts;    /* initialisation attempts so far */
    uint32_t retry_delay_ms;   /* recovery backoff (0: not scheduled yet) */
    uint32_t retry_at_ms;
} gw_dout_t;

#define GW_DOUT_RETRY_MIN_MS 1000u
#define GW_DOUT_RETRY_MAX_MS 60000u

/**
 * Load the latch with every DO off, clear the polarity inversion, then make the
 * 8 pins outputs. On a bus error the remaining steps are skipped (pins left as
 * they were), the outputs stay unusable and the error is returned.
 */
int gw_dout_init(gw_dout_t *d, const gw_dout_bus_t *bus, uint8_t active_low);

/**
 * Retry the initialisation of outputs that are not ready, at 1, 2, 4 ... 60 s
 * (GW_DOUT_RETRY_MIN_MS..MAX_MS) from the first call. Call periodically with a
 * monotonic clock. Returns true when this call made the outputs ready (all off),
 * so their state can be republished; false otherwise (not due, failed again or
 * already ready).
 */
bool gw_dout_recover(gw_dout_t *d, uint32_t now_ms);

/** Switch DO @p index (0-7). The latch is unchanged if the write fails. */
int gw_dout_set(gw_dout_t *d, uint8_t index, bool on);

/**
 * Logical DO states read back from the pins (input register). If that read
 * fails, the states written to the latch are returned and @p from_pins is false.
 */
uint8_t gw_dout_read(const gw_dout_t *d, bool *from_pins);

#ifdef __cplusplus
}
#endif
