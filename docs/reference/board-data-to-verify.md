# Board data to verify

This document lists the data of the Waveshare ESP32-S3-POE-ETH-8DI-8DO board that the firmware uses, with its source and its confidence. Waveshare publishes no schematic. Thus some data comes only from indirect evidence. Measure it on the first board.

> **Warning: Do not use this document as verified data for the board.** The firmware has never run on the board (#1, #18). The W5500, the RS485 hardware mode of the UART, the TCA9554, the GPIO DIs, the LED and the buzzer are compile-only. The real I²C access to the RTC is also compile-only.

## Sources

- The official demo `ESP32-S3-POE-ETH-8DI-8DO-Demo.zip` (Arduino, `files.waveshare.com/wiki/ESP32-S3-POE-ETH-8DI-8DO/`).
- The Waveshare wiki and the Waveshare product page.
- The ESPHome definition of this board, `Waveshare-ESP32-S3-ETH-8DI-8DO` (`esphome-devices`, `config.yaml`).
- The ESPHome definition of the sister board `Waveshare-ESP32-S3-ETH-8DI-8RO`. It has the same GPIOs for DI, I²C, RTC, LED and buzzer. Its relays are **not** inverted. Thus it is not evidence for the DO polarity.

## The data

| Data | Finding | Evidence | Confidence |
|---|---|---|---|
| DO expander | **TCA9554 at 0x20** | Demo: `TCA9554_ADDRESS 0x20` (`WS_TCA9554PWR.h`). ESPHome of the sister board: `pca9554 address: 0x20` | High |
| DO polarity | **Low level on the expander pin = DO active** (Darlington output) | **For:** the demo boots with `Dout_Init()` → `TCA9554PWR_Init(0x00, 0xFF)`, all high. A demo does not energize 8 loads at power-on. Its DI→DO mode (`WS_DIN.cpp`, `DIN_Inverse_Enable = 0`) copies the raw level: DI at rest = 1 → DO = 1 = off. The ESPHome definition of this board declares `inverted: true` on the 8 DOs. **Against:** the demo calls `Dout_Open()` at the HIGH level, and `"CH1":1` over MQTT sets the pin high. The wiki (a copy of the relay wiki) says "ALL_ON … high level (on state)" | **Medium.** You can change it: `CONFIG_GW_DO_ACTIVE_LOW` (default `y`, the most probable hypothesis). **Measure with a multimeter before you connect loads** |
| RTC battery | **A holder for a 1220 coin cell.** It accepts only a rechargeable 3 V cell (for example ML1220). The board charges the cell. The box does not contain a cell | The Waveshare product page, item 16 of "What's On Board": "RTC Battery Holder — 1220 size (12mm diameter*2mm thickness), supports only rechargeable 3V~3.3V Batt." On the sibling board ESP32-S3-RS485-CAN, the cell connects through a Schottky diode to 3V3, without a resistor | Medium. The board tests of #126 measure the voltage on the empty holder (approximately 3.0 V expected), and make sure that the RTC keeps the time without power. Refer to [The clock and the RTC battery](clock-and-rtc-battery.md) |
| RTC | **PCF85063 at 0x51** (SDA 42, SCL 41, INT 40) | Demo: `PCF85063_ADDRESS (0x51)` (`WS_PCF85063.h`). ESPHome: `platform: pcf85063` | High. The firmware uses the RTC (TIME-2). The real I²C access has no test |
| DI polarity | A GPIO with a pull-up. **Active input = level 0** (inverted) | Demo: `pinMode(DIN_PIN_CHx, INPUT_PULLUP)` and the rest state `0xFF`. ESPHome: `INPUT_PULLUP` and `inverted: true` | Medium-high. You can change it: `CONFIG_GW_DI_ACTIVE_LOW` |
| Buzzer | **Passive** (needs PWM) | Demo: `ledcAttach(46, 1000 Hz, 8 bits)` with duty 200/255. ESPHome uses `rtttl` on `ledc` | Medium. The firmware drives it with LEDC at 2.7 kHz (`CONFIG_GW_BUZZER_FREQ_HZ`). An active buzzer needs level control (`gw_bsp.buzzer.passive = false`) |
| LED | WS2812 on GPIO38, **RGB byte order** (not GRB) | Demo: `neopixelWrite(pin, green, red, blue)` (it swaps R and G). ESPHome: `rgb_order: RGB` | Medium |
| RS485 | UART1, TX 17, RX 18, RTS 21, mode `UART_MODE_RS485_HALF_DUPLEX` | Demo `WS_RS485.cpp`: `setPins(-1,-1,-1,21)` and `setMode(UART_MODE_RS485_HALF_DUPLEX)` | High |
| W5500 | SPI 13/14/15, CS 16, INT 12, RST 39 | The wiki and `WS_ETH.h` | High |
| Console | USB-C → CH34x USB-UART bridge on UART0 (GPIO 43/44) | The wiki has a link to the CH34XSER driver | Medium. If there are no logs, try `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`. The shell also operates there |

## Where the firmware keeps the data

- All the data is in [`gw_bsp.c`](https://github.com/tmijail/mqtt-rs485-gateway/blob/main/idf/components/gw_bsp/gw_bsp.c). The RTC address is `GW_PCF85063_ADDR` in [`gw_rtc.h`](https://github.com/tmijail/mqtt-rs485-gateway/blob/main/idf/components/gw_core/include/gw_rtc.h).
- The DI and DO polarities come only from indirect evidence. You can invert them in `menuconfig`, in the menu `Gateway: board`. The other data is fixed.
- At boot, `gw_dout` sets the latch of the expander to the OFF level of each channel (0xFF with active low) **before** the pins change to outputs. Thus the DOs give no pulse at boot. The firmware inverts the write and the read-back with the polarity mask.

## Strapping pins

Four strapping pins have board peripherals: GPIO0 (BOOT), GPIO3 (CAN RX, JTAG), GPIO45 (SD D0) and GPIO46 (buzzer).

- The firmware sets GPIO0 and GPIO46 only from `app_main`, never before.
- The firmware does not use GPIO3.
- GPIO45 is the D0 line of the microSD card. The firmware uses it only when `sparkplug.store_forward.sd.enabled` is `true`.

> **Caution: Read [The microSD card and GPIO45](store-and-forward.md#the-microsd-card-and-gpio45) before you set `sparkplug.store_forward.sd.enabled`.** A pull-up on D0 can stop the boot of the board.
