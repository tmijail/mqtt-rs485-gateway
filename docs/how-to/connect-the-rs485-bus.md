# Connect the RS485 bus

This procedure tells how to connect Modbus RTU devices to the RS485 port of the Waveshare ESP32-S3-POE-ETH-8DI-8DO. It also tells how to examine the bus and how to find the cause of a problem.

> **Warning: Do not use this document as verified data. Nobody tested the firmware or this procedure on the board.** The RS485 hardware mode of the UART is compile-only (#1, #18). The source of each item with the mark "Not verified on the board, see #127" is the Waveshare documentation or general RS485 practice. The board tests of #127 will correct these items.

## Before you start

You need:

- A twisted-pair cable for RS485, with a shield. Its impedance is usually 120 Ω.
- One more conductor for the common (0 V reference) of the bus.
- The data of each Modbus device: the unit ID, the baud rate, the parity and the stop bits.
- A computer with a USB-C cable for the shell of the gateway, or access to the web UI.

## The RS485 port of the board

| Item | Data | Source | Status |
|---|---|---|---|
| UART and pins | UART1: TX GPIO17, RX GPIO18, DE (RTS) GPIO21. The UART runs in the RS485 half-duplex mode | `idf/components/gw_bsp/gw_bsp.c`, Waveshare demo ([Board data to verify](../reference/board-data-to-verify.md#the-data)) | Compile-only |
| Isolation | The RS485 port is isolated from the ESP32-S3. The board has an integrated isolated power supply | Waveshare wiki: "Onboard isolated RS485 interface" | Not verified on the board, see #127 |
| Terminals | A, B and a common terminal (GND). Read the labels that are printed on the board | General RS485 practice | Not verified on the board, see #127 |
| Termination | A 120 Ω resistor between A and B, behind a jumper with the label 120R. The jumper is open at delivery | Waveshare FAQ, board review of #183 | Not verified on the board, see #127 |
| Bias (fail-safe) | The Waveshare documentation shows no bias resistors | Board review of #183 | Not verified on the board, see #127 |
| Indicator | An LED shows the TX and RX activity of the RS485 port | Waveshare wiki | Not verified on the board, see #127 |

The Waveshare FAQ tells you to move the jumper to 120R if the devices do not answer. It also says that some devices need "a 120R resistor to be connected in series". This text is not correct. A termination resistor connects between A and B, in parallel with the bus.

## Select the topology

1. Connect the devices in a line, one device after the other.
2. Keep each branch from the line to a device as short as possible.
3. Do not connect the devices in a star.
4. Connect the common terminal of each device to the common conductor.
5. Connect the shield to earth at one point only.

The RS485 standard permits 32 unit loads on one segment. The transceiver of each device sets how many unit loads it uses. For more devices or a longer cable, use a repeater.

The port of the gateway is isolated. Thus the common conductor gives the only 0 V reference between the gateway and the devices. Without it, the voltage between the two sides can be too high for the transceivers.

## Connect A and B

1. Connect the A terminal of each device to the A conductor.
2. Connect the B terminal of each device to the B conductor.
3. If no device answers, interchange A and B at the gateway.

The Modbus specification uses the names D0 and D1. Manufacturers do not use the labels A and B in the same way. Thus a bus with interchanged A and B is a frequent fault.

## Install the termination

Put one termination at each of the two ends of the line. Do not put a termination at other points.

1. Find the two devices at the ends of the line.
2. If the gateway is at one end, close the 120R jumper of the gateway.
3. If the gateway is not at an end, keep the jumper open.
4. Close the termination switch of the device at the other end. If the device has no switch, connect a 120 Ω resistor between A and B.
5. Make sure that no other device has its termination switch closed.
6. Remove the power from all devices.
7. Measure the resistance between A and B. With two terminations of 120 Ω, the result is approximately 60 Ω.

A result of approximately 120 Ω shows one termination only. A result of approximately 40 Ω or less shows three or more terminations. Bias resistors also decrease the result a little. The value of the 120R jumper is not verified on the board, see #127.

At a low baud rate and on a short cable, a bus can operate without termination. A long cable or a high baud rate causes reflections, and the gateway then counts CRC errors.

## Add bias, if necessary

When no device transmits, no transceiver drives the bus. Then noise can look like a start bit. Bias resistors keep the bus in the idle state.

- Many new transceivers have a "true fail-safe" receiver. A bus with only such transceivers does not need bias.
- If a device has a bias switch, close it at one device only.
- The Modbus specification says: put the bias at one point of the bus only. Use a resistor of 450 Ω to 650 Ω from D1 to 5 V and the same from D0 to the common.
- The Waveshare documentation shows no bias resistors on the gateway. If the bus needs bias, put it at a device or in a separate bias module. Not verified on the board, see #127.

## Configure the gateway

All devices on the bus must use the same baud rate, parity and stop bits. Set them in the `rs485` section of `config.json`:

| Key | Range | Default | Description |
|---|---|---|---|
| `baud` | 1200 to 115200 | 9600 | The baud rate of the bus |
| `parity` | `none`, `even`, `odd` | `none` | The parity |
| `stop_bits` | 1, 2 | 1 | The stop bits |
| `response_timeout_ms` | 10 to 5000 | 200 | The time that the gateway waits for the first bytes of a response |
| `retries` | 0 to 5 | 2 | The attempts after the first attempt, for a timeout, a CRC error or a frame error |
| `inter_frame_delay_ms` | 0 to 1000 | 5 | An extra delay between two transactions, after the silence of 3.5 characters |

Each device has a `unit_id` from 1 to 247. Give each physical device on the bus a different unit ID. The Modbus specification uses 2 stop bits when there is no parity. Many devices use 1 stop bit: use the values of the device.

## Examine the result

### The shell command `devices`

1. Connect the computer to the USB-C port of the board. [Board data to verify](../reference/board-data-to-verify.md#the-data) gives the console.
2. Type `devices`.

The command shows one line for each device. For example:

```
Meter1           unit   1 online  cycles 120 failed 0 | ok 120 timeout 0 crc 0 frame 0 exception 0
```

| Field | Meaning |
|---|---|
| `online` / `offline` | The state of the device. After `offline_after` failed cycles in sequence, the device is offline (DDEATH). The first good response makes it online again (DBIRTH) |
| `cycles` | The poll cycles of the device |
| `failed` | The cycles with a request that got no response after all its retries |
| `ok` | The attempts with a correct response |
| `timeout` | The attempts with no byte in `response_timeout_ms` |
| `crc` | The attempts with a response that has a bad CRC |
| `frame` | The attempts with a response that is not complete, or that is not a response to the request. A transmit failure also counts here |
| `exception` | The attempts with a Modbus exception response. The bus operates, but the device rejects the request |

The counters `ok`, `timeout`, `crc`, `frame` and `exception` count each attempt, also each retry. Thus one failed request with `retries` 2 adds 3 to `timeout`. If Modbus TCP clients send requests to the device, the line also shows `| tcp ok … timeout … crc … frame … exception …`.

The command `status` shows the summary: `network up, MQTT connected, Modbus 1/1 devices online`. Sparkplug hosts get the same counters in the DataSet `Node Info/Modbus` of the node.

### One read of a register

1. Type `mbread <unit> <coil|discrete|holding|input> <address> [count]`. For example: `mbread 1 holding 10 2`.
2. Read the result. For example: `mbread: unit 1 holding 10: 123 0`.

The address is the address in the Modbus frame, from 0. The command also gives `timeout`, `CRC error`, `framing error` or `exception <code>`. It is available when the build has `CONFIG_GW_SHELL_WRITE_COMMANDS` (the default).

The test read in the web UI (#182) will do the same from the device table, without the shell.

### The status LED

| Pattern | Meaning |
|---|---|
| Green, continuous | Normal operation: all devices are online |
| Green and orange, alternately (1 Hz) | One or more Modbus devices are offline |

The other patterns are in [The status signals](../reference/status-signals.md#status-led). The LED is compile-only (#18).

## If there is a problem

| Symptom | Probable cause | Action |
|---|---|---|
| All devices show `timeout`, and `ok` stays at 0 | A and B are interchanged. The baud rate, the parity or the stop bits are not correct. The common is not connected. The devices have no power | Interchange A and B at the gateway. Compare the `rs485` values with each device. Connect the common |
| All devices show `timeout`, and the RS485 TX LED stays off | The gateway does not transmit | Type `status` and read the configuration line. Report the result in #127 |
| `crc` errors only at a high baud rate | Reflections on the bus. There is no termination, or it is at the wrong point. A branch is too long. The cable is not twisted pair | Install the termination at the two ends only. Make the branches shorter. Decrease `baud` |
| `frame` errors, mostly before the first byte of a response | The bus floats when no device transmits. Noise. A second master on the bus | Add bias at one point. Make sure that the gateway is the only master |
| Errors only when a motor or a frequency converter runs | Electrical noise | Connect the common and the shield as this procedure tells. Install the RS485 cable away from power cables |
| Some `timeout` errors, but most attempts are `ok` | The device is slow | Increase `response_timeout_ms`. Increase `inter_frame_delay_ms` |
| One device shows `timeout`, the other devices are `online` | The unit ID or the serial values of this device are not correct. Two devices use the same unit ID. Its A and B are interchanged. Its termination switch is closed in the middle of the line | Examine the unit ID and the serial values of the device. Examine its connection |
| `exception` errors, but no `timeout` | The bus operates. The address, the area or the function of a tag is not correct for the device | Compare the tags with the register map of the device. Use `mbread` to read the register |
