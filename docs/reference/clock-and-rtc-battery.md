# The clock and the RTC battery

This document gives the facts about the clock of the gateway (TIME-1, TIME-2, TIME-3, issue #160): the RTC battery, the sources of the clock, and what the gateway shows while it waits for a valid clock.

- For the procedure, refer to [Fit the RTC battery](../how-to/fit-the-rtc-battery.md).
- For the reasons and the choices of the implementation, refer to [`idf/README.md` §6.4](../../idf/README.md#64-clock).

> **Warning: Do not use this document as verified data for the board.** The firmware has never run on the board (#1, #18). The real I²C access to the RTC is compile-only. The data of the holder comes from the Waveshare product page, not from a measurement (#126).

## The RTC and its battery

| Item | Value |
|---|---|
| RTC | PCF85063 at I²C address 0x51 (SDA GPIO 42, SCL GPIO 41, INT GPIO 40) |
| Battery holder | For a 1220 coin cell: 12 mm diameter, 2 mm thickness |
| Cell | A rechargeable 3 V cell, for example an ML1220. The box of the board does not contain a cell |
| Cell that you must not use | CR1220 or another primary cell. The board charges the cell |
| Charge circuit | Waveshare publishes no schematic. On the sibling board ESP32-S3-RS485-CAN, the cell connects through a Schottky diode to 3V3, without a resistor. The board tests of #126 measure the voltage on the empty holder (approximately 3.0 V expected) |
| Necessary | Yes (decision of the product owner, issue #160). Without the cell, the gateway sends no data after a power cut until SNTP or an operator sets the time |

## The sources of the clock

| Source | `time.source` | Valid for timestamps | `Node Info/Time Synced` |
|---|---|---|---|
| None (the clock counts from 1970) | `none` | No | `false` |
| The RTC at boot, if its OS bit is clear and its time is from 2025 to 2099 | `rtc` | Yes | `false` |
| The browser (`POST /api/time`, the button "Set the time from the browser") | `browser` | Yes | `false` |
| SNTP | `sntp` | Yes | `true` |

The gateway writes the time into the RTC after each SNTP synchronization and after each time from the browser. No source makes the clock not valid again.

## Without a valid clock

In normal mode, the gateway opens no MQTT session until the clock is valid. There is no time limit.

| Item | Behaviour |
|---|---|
| MQTT | No CONNECT, no NBIRTH, no DBIRTH, no NDATA, no DDATA |
| Store & forward | Stores nothing |
| Polling, web UI, REST API, shell | Operate |
| Safe state of the DOs (IO-3) | Applies, as in each period without a Sparkplug session |
| bdSeq | The wait uses no value. The first CONNECT after the wait has bdSeq 0 |
| Backoff, broker failover, interface selection | The wait is not a failed attempt |
| After the wait | The gateway connects at once. The first births carry the current values with UTC timestamps. There is no rebirth |

## The signals

| Where | While the gateway waits for a valid clock | When the RTC lost the time at boot (OS bit) |
|---|---|---|
| Log | `edge: waiting for a valid clock: no MQTT connection and no NBIRTH until then. Make sure that the gateway can reach the SNTP server (ntp.server), or use "Set the time from the browser" in the web UI (Status tab)`, at the start of the wait and every 10 min. At the end: `edge: clock valid (source …) after a wait of N s: connecting` | `io: RTC: the RTC lost the time while the power was off (OS bit): examine the RTC battery (a rechargeable 3 V cell, for example ML1220; never a CR1220)`, at boot. While the gateway waits, the same text also comes from `edge` |
| Shell `status` | `network  up, MQTT waiting for a valid clock, …`; `time … NOT valid (no SNTP, RTC or browser time yet)`; a `clock` line with the text of the log | A `battery` line with the text of the log, until the next boot |
| `GET /api/status` | `time.valid` `false`, `time.source` `none`, `time.waiting` `true` | `time.rtc.lost` `true`, until the next boot |
| Web UI, Status tab | A banner "Waiting for a valid clock", and "MQTT waiting for a valid clock" | A banner that tells you to examine the RTC battery |
| Status LED | Magenta, blinks at 1 Hz | — |
| Buzzer | The alarm `mqtt_down` (2 beeps each 10 s), if it is enabled: there is no Sparkplug session | — |
