# The clock and the RTC battery

This document gives the facts about the clock of the gateway (issue #160): the RTC battery, the sources of the clock, and what the gateway shows while it waits for a valid clock.

- For the procedure, refer to [Fit the RTC battery](../how-to/fit-the-rtc-battery.md).
- To set the SNTP servers, refer to [Set the time servers](../how-to/set-the-time-servers.md).
- For the Sparkplug reasons of the wait, refer to [No session without a valid clock](sparkplug-behavior.md#no-session-without-a-valid-clock-time-1-160).
- For the other limits, refer to [Known limitations](known-limitations.md#the-clock-time-1-time-2).

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

### The rules of the sources

- **Valid.** The clock is valid when a source set it. The gateway does not compare the date with a limit for this decision. `gw_clock` holds this logic, with host tests.
- **The RTC at boot.** The gateway reads the PCF85063 before the configuration loads. It uses the time if the OS bit (oscillator stopped) is clear, the BCD fields are valid, and the year is from 2025 to 2099.
- **The I²C bus.** The RTC and the TCA9554 share one I²C bus with one owner: `app_main` before the `local_io` task starts, and then that task.
- **The RTC write.** The gateway writes the RTC after each SNTP synchronization and after each time from the browser. This write also clears the OS bit.
- **The RTC time is not synchronized.** The RTC drifts some seconds per day, and nothing corrects it until SNTP. Thus `Node Info/Time Synced` (TIME-3) stays `false`.
- **With the RTC time,** the gateway connects at once and does not wait for SNTP. TLS uses that time to check the certificate dates.
- **The hint of the battery** stays until the next boot, also when another source sets the clock. The next power cut loses the time again.
- **Recovery mode and commissioning.** The gateway also reads the RTC at boot and writes it after SNTP. The reduced `local_io` task of these modes does the I²C access (#74).

### SNTP

- SNTP starts with the first address of an interface. Until the first synchronization, each new address starts SNTP again, and one request goes out at once.
- Reason: with `network.ipv6`, the static IPv6 address can come before the DHCP IPv4 address. The first request to an IPv4 server then fails, and lwIP waits 15 s or more for the next one.
- The SNTP client gets all the servers of `ntp.servers`, 3 at most (`CONFIG_LWIP_SNTP_MAX_SERVERS`, #201).
- lwIP asks the next server after 15 s without an answer (`SNTP_RECV_TIMEOUT`). Thus a first server that does not answer delays the first time by approximately 15 s. lwIP does not tell which server answered.
- The random start delay of the lwIP SNTP client is disabled.

### The time from the browser

- The button "Set the time from the browser" (Status tab) sends `POST /api/time` with `{"utc_ms": <ms UTC>}`, the time of the computer. The request has the CSRF and password checks of each write.
- The gateway sets the system clock and writes the RTC. The source becomes `browser`, and in normal mode the gateway connects.
- When SNTP synchronized the clock, the gateway refuses a browser time with `409`.
- The request also operates in recovery mode and in commissioning: the RTC keeps the time for the next boot.
- The error of this time is the error of the clock of the computer plus the network delay. Usually it is less than 1 s.

## Without a valid clock

In normal mode, the gateway opens no MQTT session until the clock is valid. There is no time limit.

| Item | Behaviour |
|---|---|
| MQTT | No CONNECT, no NBIRTH, no DBIRTH, no NDATA, no DDATA |
| Store & forward | Stores nothing |
| Polling, web UI, REST API, shell | Operate |
| [Safe state of the DOs](configuration.md#the-safe-state-of-the-dos) | Applies, as in each period without a Sparkplug session |
| bdSeq | The wait uses no value. The first CONNECT after the wait has bdSeq 0 |
| Backoff, broker failover, interface selection | The wait is not a failed attempt |
| After the wait | The gateway connects at once. The first births carry the current values with UTC timestamps. There is no rebirth |
| Bus and watchdog | The wait does not hold the bus or the watchdog, and it uses no memory. SNTP and the browser time wake the Sparkplug task |

## The signals

| Where | While the gateway waits for a valid clock | When the RTC lost the time at boot (OS bit) |
|---|---|---|
| Log | `edge: waiting for a valid clock: no MQTT connection and no NBIRTH until then. Make sure that the gateway can reach an SNTP server (ntp.servers), or use "Set the time from the browser" in the web UI (Status tab)`, at the start of the wait and every 10 min. At the end: `edge: clock valid (source …) after a wait of N s: connecting` | `io: RTC: the RTC lost the time while the power was off (OS bit): examine the RTC battery (a rechargeable 3 V cell, for example ML1220; never a CR1220)`, at boot. While the gateway waits, the same text also comes from `edge` |
| Shell `status` | `network  up, MQTT waiting for a valid clock, …`; `time … NOT valid (no SNTP, RTC or browser time yet)`; a `clock` line with the text of the log | A `battery` line with the text of the log, until the next boot |
| `GET /api/status` | `time.valid` `false`, `time.source` `none`, `time.waiting` `true` | `time.rtc.lost` `true`, until the next boot |
| Web UI, Status tab | A banner "Waiting for a valid clock", and "MQTT waiting for a valid clock" | A banner that tells you to examine the RTC battery |
| Status LED | Magenta, blinks at 1 Hz | — |
| Buzzer | The alarm `mqtt_down` (2 beeps each 10 s), if it is enabled: there is no Sparkplug session | — |

## Compile-only parts

- The real I²C read and write of the PCF85063, and its 24-hour mode.
- The firmware assumes the reset value of the control register, and does not change `CAP_SEL`.
- In QEMU, a simulated PCF85063 keeps the time in RTC memory. The time stays after a software restart. It is lost with the QEMU process, with the OS bit set as at power-on.
