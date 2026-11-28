/*
 * Issue #300: the second-stage bootloader sets the DE pin of the RS485 transceiver to its
 * inactive level, as its first step.
 *
 * On the board an isolator connects GPIO21 to DE of the SP3485E, and an open isolator input gives
 * a high DE: while GPIO21 is not an output, the transceiver drives the bus (#287). The
 * application sets DE at the start of app_main (gw_rs485_de_off); this hook does the same thing
 * earlier, so the interval where the gateway holds the bus is only the reset, the ROM and the
 * start of this bootloader. The ROM part stays: no firmware runs there.
 *
 * Limit: an OTA update writes only an application slot, never the bootloader. Only a board that
 * gets the bootloader through USB (idf.py flash, the install tool of the site) has this hook.
 *
 * Register-level only: the bootloader has no GPIO driver. The order follows gpio_set_level() then
 * gpio_output_enable() of the driver, with the level first, so the pad becomes an output already
 * at the inactive level and DE gets no pulse.
 */
#include <stdint.h>

#include "esp_log.h"
#include "gw_bsp_pins.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/io_mux_reg.h"
#include "soc/soc_caps.h"

_Static_assert(GW_BSP_RS485_DE_GPIO >= 0 && GW_BSP_RS485_DE_GPIO < SOC_GPIO_PIN_COUNT &&
                   ((SOC_GPIO_VALID_OUTPUT_GPIO_MASK >> GW_BSP_RS485_DE_GPIO) & 1ULL) != 0,
               "GW_BSP_RS485_DE_GPIO is not an output GPIO of this chip");

#define DE_OFF_LEVEL (GW_BSP_RS485_DE_ACTIVE_LOW ? 1U : 0U)

static const char *TAG = "gw_boot";

/* The bootloader links with "-u bootloader_hooks_include": this symbol makes the linker take
 * this object, and with it the strong definitions of the weak hooks below. */
void bootloader_hooks_include(void);
void bootloader_before_init(void);
void bootloader_after_init(void);

void bootloader_hooks_include(void)
{
}

/* Called before bootloader_init(): no BSS yet, no console of the bootloader yet. Only register
 * writes here; the log line comes from bootloader_after_init(). */
void bootloader_before_init(void)
{
    const uint32_t de = GW_BSP_RS485_DE_GPIO;
    gpio_ll_set_level(&GPIO, de, DE_OFF_LEVEL);
    gpio_ll_set_output_signal_matrix_source(&GPIO, de, SIG_GPIO_OUT_IDX, false);   /* GPIO_OUT */
    gpio_ll_set_output_enable_ctrl(&GPIO, de, false, false);   /* the enable from GPIO_ENABLE */
    gpio_ll_func_sel(&GPIO, (uint8_t)de, PIN_FUNC_GPIO);   /* the pad takes the GPIO matrix */
    gpio_ll_output_enable(&GPIO, de);
}

static uint32_t reg_bit(uint32_t reg_low, uint32_t reg_high, uint32_t gpio)
{
    return gpio < 32 ? (REG_READ(reg_low) >> gpio) & 1U : (REG_READ(reg_high) >> (gpio - 32)) & 1U;
}

/* After bootloader_init() the console of the bootloader works (on the board: the USB-Serial-JTAG).
 * The line also gives the two register bits as they are after bootloader_init(): on the board,
 * "output enable 1, output N" (N: the inactive level) shows that nothing in the init changed the
 * pin. QEMU does not model these GPIO registers (they read 0), so there the bits say nothing. */
void bootloader_after_init(void)
{
    const uint32_t de = GW_BSP_RS485_DE_GPIO;
    ESP_LOGI(TAG, "DE GPIO%u set %s before the init of the bootloader; after the init: output "
             "enable %u, output %u", (unsigned)de, DE_OFF_LEVEL ? "high" : "low",
             (unsigned)reg_bit(GPIO_ENABLE_REG, GPIO_ENABLE1_REG, de),
             (unsigned)reg_bit(GPIO_OUT_REG, GPIO_OUT1_REG, de));
}
