# Board data to verify

This document lists the data of the Waveshare ESP32-S3-POE-ETH-8DI-8DO board that the firmware uses, with its source and its confidence. Waveshare publishes no schematic. Thus some data came first from indirect evidence. The inspection of the first board (#18) confirmed part of it.

> **Warning: Do not use this document as verified data for the firmware on the board.** The firmware has never run on the board (#1, #18). The inspection used photos, a multimeter, `espefuse summary` and the demo firmware of Waveshare on the board. The W5500, the RS485 hardware mode of the UART, the TCA9554, the GPIO DIs, the LED and the buzzer are compile-only in this firmware.

## Sources

- The inspection of the first board, before the first flash (#18). The [inspection of the first board](#the-inspection-of-the-first-board) gives what it found.
- The official demo `ESP32-S3-POE-ETH-8DI-8DO-Demo.zip` (Arduino, `files.waveshare.com/wiki/ESP32-S3-POE-ETH-8DI-8DO/`). The board arrives with a version of this demo in its flash.
- The Waveshare wiki and the Waveshare product page.
- The ESPHome definition of this board, `Waveshare-ESP32-S3-ETH-8DI-8DO` (`esphome-devices`, `config.yaml`).
- The ESPHome definition of the sister board `Waveshare-ESP32-S3-ETH-8DI-8RO`. It has the same GPIOs for DI, I²C, RTC, LED and buzzer. Its relays are **not** inverted. Thus it is not evidence for the DO polarity.

## The data

| Data | Finding | Evidence | Confidence |
|---|---|---|---|
| Module | **ESP32-S3-WROOM-1U-N16R8**: 16 MB of flash, 8 MB of octal PSRAM, an external antenna on an SMA connector | The marking of the module. The eFuses: `PSRAM_CAP` 8M, `PSRAM_VENDOR` AP_3v3. Chip revision v0.2 | High (first board) |
| DO expander | **TCA9554 at 0x20** | The marking `PW554` (TI, TCA9554PWR). The A version (PW554A) has the address 0x38. Demo: `TCA9554_ADDRESS 0x20` (`WS_TCA9554PWR.h`) | High. The first I²C access confirms the address pins |
| DO polarity | **Low level on the expander pin = DO active** | **First board:** the demo sets all the pins of the expander high at boot, and the 8 DO LEDs stay off. A DI connected to DGND turns on its DO: the DI→DO mode of the demo (`DIN_Inverse_Enable = 0`) copies the raw level. **Earlier evidence:** ESPHome declares `inverted: true` on the 8 DOs | **High, not measured.** The setting is `CONFIG_GW_DO_ACTIVE_LOW` (default `y`). **Measure each output with this firmware before you connect loads (#1)** |
| DO output stage | Two quad optocouplers EL3H7, then a ULN2803A (Darlington, sink). The terminals are DO1–DO8, DGND and DOCOM | The markings of the parts and the labels of the terminals | High (first board). Refer to [Outputs and inputs](#outputs-and-inputs) |
| DI polarity | A GPIO with a pull-up. **Active input = level 0** (inverted) | **First board:** a DI connected to DGND turns on the DO of the same number in the demo, which copies the raw level. Thus the GPIO is low. Demo: `INPUT_PULLUP` and the rest state `0xFF`. ESPHome: `inverted: true` | High, not measured with this firmware. The setting is `CONFIG_GW_DI_ACTIVE_LOW` |
| DI input stage | Two quad optocouplers EL3H4 (AC input), with an internal isolated supply on DICOM. The terminals are DI1–DI8, DGND and DICOM | The markings of the parts. The tests of the first board | High (first board). Refer to [Outputs and inputs](#outputs-and-inputs) |
| RTC battery | **A holder for a 1220 coin cell.** It accepts only a rechargeable 3 V cell (for example ML1220). The board charges the cell. The box does not contain a cell | **First board:** the empty holder gives approximately 3.0 V with the power on (#126, L11, step 1). The product page: "RTC Battery Holder — 1220 size, supports only rechargeable 3V~3.3V Batt." | Medium. The charge path and the charge time are not known (#126, L11). Refer to [The clock and the RTC battery](clock-and-rtc-battery.md) |
| RTC | **PCF85063 at 0x51** (SDA 42, SCL 41, INT 40) | **First board:** a part with the marking `063A` and a crystal, probably the PCF85063A. The demo prints "PCF85063 is running". Demo: `PCF85063_ADDRESS (0x51)` | High. The firmware uses the RTC (TIME-2). The I²C access of this firmware has no test |
| Buzzer | **Passive** (needs PWM) | **First board:** an S8050 transistor (marking J3Y) drives it, and it sounds with the demo. Demo: `ledcAttach(46, 1000 Hz, 8 bits)`. ESPHome uses `rtttl` on `ledc` | Medium. The type and the best frequency are not measured (#126, L6). The firmware uses LEDC at 2.7 kHz (`CONFIG_GW_BUZZER_FREQ_HZ`) |
| LED | WS2812 on GPIO38, **RGB byte order** (not GRB) | **First board:** the LED is near the BOOT button, and the demo shows green, then blue. Demo: `neopixelWrite(pin, green, red, blue)`. ESPHome: `rgb_order: RGB` | Medium. The color order of this firmware is not tested |
| RS485 | UART1, TX 17, RX 18, RTS 21, mode `UART_MODE_RS485_HALF_DUPLEX`. Transceiver SP3485E, isolated. Terminals A+, B− and a ground (⏚). A jumper `NC`/`120R` connects 120 Ω across A and B | **First board:** the markings, the labels, and the resistance across A and B (approximately 120 Ω with `120R`, approximately 48 kΩ with `NC`). Demo `WS_RS485.cpp`: `setPins(-1,-1,-1,21)` | High |
| CAN | TJA1051T/3, isolated. Terminals CAN_H, CAN_L and a ground (⏚), with its own jumper `NC`/`120R` | The marking and the labels. The firmware does not use CAN | High (first board) |
| W5500 | SPI 13/14/15, CS 16, INT 12, RST 39 | The wiki and `WS_ETH.h`. The marking on the first board | High |
| Console | **USB-C → USB-Serial-JTAG of the ESP32-S3** (VID:PID 303A:1001). The board has no USB-UART bridge. UART0 (GPIO 43/44) is on TX0 and RX0 of the pin header | **First board:** Windows shows "USB JTAG/serial debug unit" and a "USB Serial Device", with no extra driver. The firmware uses `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` | High. The shell of this firmware on the USB is not tested yet (#18) |

## The inspection of the first board

The inspection of 2026-11-26 occurred before the first flash of this firmware. Its procedure and its results are in #18.

### Pin header, expansion terminals and jumpers

- **Pin header (2 × 13).** One row has `5V 5V G G 3V3 G SCL SDA 3V3 G 45 47 48`. The other row has `VBUS G DP DN 3V3 G TX0 RX0 3V3 G 0 1 38`. The labels are not exactly above their pins: count from the end of the row.
- **GPIO1** is on the pin header, and the firmware does not use it.
- **Expansion terminals.** The order is `IO1 IO0 RXD TXD SDA SCL GND VCC`. A jumper `5V`/`VCC`/`3V3` sets the voltage of VCC. SDA and SCL are the I²C bus of the TCA9554 and of the RTC.
- **Jumper `NC`/`USB-OUT`.** Keep it on `NC`. The wiki says that the other position supplies power through the USB-C connector: do not connect a computer then.
- **Antenna.** The connector is an SMA female. The box contains the antenna.

### Isolation

The board has four parts that are isolated from each other. The tests measured continuity between the terminals without power.

| Part | Terminals |
|---|---|
| Logic | `−` of the 7–36 V input, `G` of the pin header, `GND` of the expansion terminals |
| DI side | `DGND` and `DICOM` of the DI terminals |
| DO side | `DGND` and `DOCOM` of the DO terminals |
| Bus side | The ground (⏚) of RS485 and the ground (⏚) of CAN. These two are connected |

Two isolated converters B0505LS-1W supply the isolated parts. The continuity between the DI side and the bus side is not measured.

### Outputs and inputs

- **Outputs.** The ULN2803A sinks the current of the load to `DGND` of the DO side. Connect each load between the positive of its supply and its DO terminal. Connect `DOCOM` to the positive of the supply of the loads, for the freewheel diodes. Connect the negative of that supply to `DGND` of the DO side.
- **Inputs.** `DICOM` is an internal isolated supply of approximately 4.5 V. A free DI is at approximately 3.8 V. A contact between a DI and `DGND` of the DI side turns on its input. A connection between a DI and `DICOM` does not.
- The inputs are bidirectional (AC-input optocouplers). The connection of an external active signal is not tested yet.

## Where the firmware keeps the data

- All the data is in [`gw_bsp.c`](https://github.com/tmijail/mqtt-rs485-gateway/blob/main/idf/components/gw_bsp/gw_bsp.c). The RTC address is `GW_PCF85063_ADDR` in [`gw_rtc.h`](https://github.com/tmijail/mqtt-rs485-gateway/blob/main/idf/components/gw_core/include/gw_rtc.h).
- You can invert the DI and DO polarities in `menuconfig`, in the menu `Gateway: board`. The other data is fixed.
- At boot, `gw_dout` sets the latch of the expander to the OFF level of each channel (0xFF with active low) **before** the pins change to outputs. Thus the DOs give no pulse at boot. The firmware inverts the write and the read-back with the polarity mask.

## Strapping pins

Four strapping pins have board peripherals: GPIO0 (BOOT), GPIO3 (CAN RX, JTAG), GPIO45 (SD D0) and GPIO46 (buzzer).

- The firmware sets GPIO0 and GPIO46 only from `app_main`, never before.
- The firmware does not use GPIO3.
- GPIO45 is the D0 line of the microSD card. The board has a pull-up of approximately 10 kΩ from GPIO45 to 3V3. Thus GPIO45 is high at each reset.
- On the first board, the eFuses `VDD_SPI_FORCE` and `VDD_SPI_TIEH` were burned at the factory of the module. `espefuse summary` gives "Flash voltage (VDD_SPI) set to 3.3V by efuse". Thus the ROM ignores GPIO45, and the board boots.
- The firmware uses GPIO45 only when `sparkplug.store_forward.sd.enabled` is `true`.

The pull-up is on the board, so the card and the spill do not change the level of GPIO45 at reset. Refer to [The microSD card and GPIO45](store-and-forward.md#the-microsd-card-and-gpio45).
