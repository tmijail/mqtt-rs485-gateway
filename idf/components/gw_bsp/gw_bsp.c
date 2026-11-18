#include "gw_bsp.h"

#include "sdkconfig.h"

/* The two levels that the available sources only establish indirectly can be
 * flipped from menuconfig ("Gateway: board"); every other fact is fixed. */
#if CONFIG_GW_DI_ACTIVE_LOW
#define DI_ACTIVE_LOW 0xFF
#else
#define DI_ACTIVE_LOW 0x00
#endif
#if CONFIG_GW_DO_ACTIVE_LOW
#define DO_ACTIVE_LOW 0xFF
#else
#define DO_ACTIVE_LOW 0x00
#endif

const gw_bsp_t gw_bsp = {
    .name = "Waveshare ESP32-S3-POE-ETH-8DI-8DO",
    /* Opto-isolated inputs: the energised optocoupler pulls the pulled-up GPIO low. */
    .di = {
        .gpio = {GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7,
                 GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_10, GPIO_NUM_11},
        .active_low = DI_ACTIVE_LOW,
        .pull_up = DI_ACTIVE_LOW != 0,
    },
    /* Shared with the PCF85063 RTC at 0x51 (TIME-2; its address is in gw_rtc.h). */
    .i2c = {.port = -1, .sda = GPIO_NUM_42, .scl = GPIO_NUM_41, .clk_hz = 100000},
    /* Darlington sink outputs on TCA9554 EXIO1..8: a LOW expander pin turns the
     * output on (demo: TCA9554PWR_Init(0x00, 0xFF) at boot; ESPHome: inverted). */
    .dout = {.i2c_addr = 0x20, .active_low = DO_ACTIVE_LOW},
    /* Isolated transceiver; the UART's RS485 half-duplex mode raises RTS = DE while sending. */
    .rs485 = {.uart = UART_NUM_1, .tx = GPIO_NUM_17, .rx = GPIO_NUM_18, .de = GPIO_NUM_21,
              .de_active_low = false},
    .w5500 = {.host = SPI2_HOST, .mosi = GPIO_NUM_13, .miso = GPIO_NUM_14, .sclk = GPIO_NUM_15,
              .cs = GPIO_NUM_16, .irq = GPIO_NUM_12, .rst = GPIO_NUM_39, .clk_hz = 20000000},
    /* The demo swaps the arguments of neopixelWrite(): the LED takes R, G, B. */
    .status_led = {.gpio = GPIO_NUM_38, .order = GW_BSP_LED_ORDER_RGB},
    .buzzer = {.gpio = GPIO_NUM_46, .passive = true},
    /* microSD slot, 1-bit SDMMC. D0 on GPIO45 is the VDD_SPI strapping pin: a pull-up that
     * holds it high at reset selects 1.8 V for the flash (docs/reference/store-and-forward.md#the-microsd-card-and-gpio45). */
    .sd = {.clk = GPIO_NUM_48, .cmd = GPIO_NUM_47, .d0 = GPIO_NUM_45},
    /* BOOT, a strapping pin: pulled up on the board, low while pressed; read only after boot */
    .boot_button = {.gpio = GPIO_NUM_0, .active_low = true},
};
