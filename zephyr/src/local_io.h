/*
 * Local digital I/O: 8 opto-isolated inputs on SoC GPIOs and 8 sinking
 * outputs behind the TCA9554 I2C expander (IO-1, IO-2). The same thread
 * also animates the status LED (IO-5).
 *
 * Pins come from the devicetree: /zephyr,user { di-gpios; do-gpios; }.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GW_LOCAL_IO_H_
#define GW_LOCAL_IO_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/gpio.h>

/** Configure the pins (all outputs OFF) without starting the sampler. */
int local_io_init(void);

/** Start DI sampling at 1 kHz with the given debounce time. */
int local_io_start(uint16_t debounce_ms);

/**
 * Drive output @p channel (0-based) and publish the new state through the
 * model once the I2C write is acknowledged. Safe to call from any thread.
 */
int local_io_set_output(uint8_t channel, bool on);

/** Devicetree spec of input @p channel (used by the simulator shell). */
const struct gpio_dt_spec *local_io_di_spec(uint8_t channel);

#endif /* GW_LOCAL_IO_H_ */
