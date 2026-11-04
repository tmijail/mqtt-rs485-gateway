/*
 * Board support: the one declarative description of the Waveshare
 * ESP32-S3-POE-ETH-8DI-8DO. Every pin, bus address and active level of the
 * board lives in gw_bsp (gw_bsp.c); the drivers (gw_board, gw_rs485, gw_net)
 * read it and contain no board facts of their own. The evidence for each fact
 * and its confidence level are in README section 2.
 *
 * Active levels of the 8-channel groups are bit masks: bit n set means channel
 * n+1 is active when its pin is LOW (see gw_dout.h for the conversion).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "driver/spi_common.h"
#include "driver/uart.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_BSP_IO_CHANNELS 8

typedef enum {
    GW_BSP_LED_ORDER_RGB = 0,   /* bytes sent as R, G, B */
    GW_BSP_LED_ORDER_GRB,       /* bytes sent as G, R, B (WS2812 datasheet order) */
} gw_bsp_led_order_t;

typedef struct {
    const char *name;
    struct {
        gpio_num_t gpio[GW_BSP_IO_CHANNELS];   /* DI1..DI8 */
        uint8_t active_low;                    /* bit n: DI n+1 asserted when the pin reads 0 */
        bool pull_up;                          /* enable the internal pull-up */
    } di;
    struct {
        i2c_port_num_t port;                   /* -1: any free controller */
        gpio_num_t sda;
        gpio_num_t scl;
        uint32_t clk_hz;
    } i2c;
    struct {
        uint8_t i2c_addr;                      /* TCA9554; DO n+1 = expander pin n */
        uint8_t active_low;                    /* bit n: DO n+1 energised by a LOW pin */
    } dout;
    struct {
        uart_port_t uart;
        gpio_num_t tx;
        gpio_num_t rx;
        gpio_num_t de;                         /* transceiver DE, driven as the UART RTS */
        bool de_active_low;
    } rs485;
    struct {
        spi_host_device_t host;
        gpio_num_t mosi;
        gpio_num_t miso;
        gpio_num_t sclk;
        gpio_num_t cs;
        gpio_num_t irq;
        gpio_num_t rst;
        uint32_t clk_hz;
    } w5500;
    struct {
        gpio_num_t gpio;                       /* single WS2812 */
        gw_bsp_led_order_t order;
    } status_led;
    struct {
        gpio_num_t gpio;
        bool passive;                          /* needs a PWM tone (factory reset beeps) */
    } buzzer;
    struct {
        gpio_num_t gpio;                       /* CFG-5 factory reset, NET-5 access point */
        bool active_low;
    } boot_button;
} gw_bsp_t;

extern const gw_bsp_t gw_bsp;

#ifdef __cplusplus
}
#endif
