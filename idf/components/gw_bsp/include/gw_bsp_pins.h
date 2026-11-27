/*
 * The board facts that code outside the application also needs, as plain macros: no ESP-IDF
 * header, no type. gw_bsp.c builds gw_bsp from them, so they are the one source of the facts.
 *
 * Issue #300: the second-stage bootloader (bootloader_components/gw_boot_de) sets the RS485 DE
 * pin to its inactive level before the application starts. The bootloader is a separate build
 * with no driver component, so it cannot include gw_bsp.h (driver/gpio.h, driver/uart.h); it
 * includes this header only. Keep this header free of ESP-IDF includes.
 */
#pragma once

/* The DE input of the SP3485E, through the isolator (HOPERF CMT8263N1). An open isolator input
 * gives a high DE (#287): the pin must be an output at the inactive level. */
#define GW_BSP_RS485_DE_GPIO       21
#define GW_BSP_RS485_DE_ACTIVE_LOW 0   /* 0: DE active high; the UART RS485 mode raises RTS = DE */
