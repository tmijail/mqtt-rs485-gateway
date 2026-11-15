# The status signals

This document lists the signals that the gateway gives at the panel: the modes, the status LED, the buzzer and the BOOT button. The source is [`idf/README.md` §3.4 and §3.5](../../idf/README.md#34-configuration). For the configuration of the alarms, refer to [`local_io.buzzer`](configuration.md#local_iobuzzer).

> **Warning: Do not use this document as verified data for the board.** The firmware has never run on the board (#1, #18). The LED and the buzzer are compile-only. The E2E reads the patterns from the log of the firmware in QEMU.

## Modes

| Mode | When | What runs |
|---|---|---|
| `normal` | The configuration is valid | All |
| `recovery` | The configuration is rejected, or 3 crashes in sequence (safe mode) | The network, the web UI, the shell, the access point and the firmware upload. No polling, no MQTT |
| `commissioning` | After a factory reset: no configuration | The network (DHCP), the web UI, the shell and the access point |
| `fault` | The boot did not find the PSRAM | The network, the web server and the shell. The gateway does not start |

`GET /api/status` (`mode`), the card **Gateway** of the web UI and the shell command `status` give the mode.

## Status LED

The LED shows the pattern of the highest state in the table.

| State | Pattern |
|---|---|
| Factory reset in progress | White, continuous |
| BOOT pushed for more than 2 s | White, blinks at 4 Hz |
| Claim window open, or access point open (no key) | Blue and white, alternately (each color 250 ms) |
| Commissioning | Blue, blinks at 1 Hz |
| Configuration not valid (recovery mode) | Red, blinks at 4 Hz |
| No network (no IP address) | Red, continuous |
| No valid clock: the MQTT session waits | Magenta, blinks at 1 Hz |
| No broker | Amber, blinks at 1 Hz |
| A Modbus device is offline | Green and orange, alternately (1 Hz) |
| Normal operation | Green, continuous |

While the access point runs with WPA2, a short cyan flash (125 ms) each 2 s replaces the pattern, from "Commissioning" to "Normal operation". The log gives the name of the pattern, for example `led: status: operating, access point`.

## Buzzer

One beep is 150 ms of tone and 150 ms of silence.

| Signal | When |
|---|---|
| 1 beep | You release BOOT after 2 to 10 s: the access point starts (and the claim window opens, if no web password is set) |
| 1 beep | The claim window set the first web password |
| 2 beeps | BOOT held for 10 s: the factory reset starts |
| 1 beep each 10 s | Alarm `device_offline`: one or more Modbus devices are offline |
| 2 beeps each 10 s | Alarm `mqtt_down`: no Sparkplug session |
| 3 beeps each 10 s | Alarm `config_error`: the configuration is rejected |

The alarms sound only when `local_io.buzzer.alarms` contains them. `mqtt_down` and `device_offline` sound after `local_io.buzzer.alarm_after_s` (30 s by default). `config_error` sounds immediately. If a number of alarms are active, the alarm with the most beeps sounds.

## BOOT button

The firmware reads the button only after the boot. It must see the button released one time.

| Push | Result |
|---|---|
| Less than 2 s | Nothing |
| 2 to 10 s | The access point starts, or its times start again. Without a web password, the claim window also opens |
| More than 10 s | Factory reset: the gateway erases the configuration, the secrets and the commissioning password. Then it restarts in commissioning mode |
| Held at power-on | The ROM starts the download mode for a flash over USB. The firmware does not start |

For the access point, refer to [The access point](access-point.md). For the clock signals, refer to [The clock and the RTC battery](clock-and-rtc-battery.md).
