# The status signals

This document lists the signals that the gateway gives at the panel: the modes, the status LED, the buzzer and the BOOT button. For the details of the modes, refer to [Commissioning and secrets](commissioning-and-secrets.md#the-modes). For the configuration of the alarms, refer to [`local_io.buzzer`](configuration.md#local_iobuzzer).

> **Warning: Do not use this document as verified data for the board.** The firmware has never run on the board (#1, #18). The LED and the buzzer are compile-only. The E2E reads the patterns from the log of the firmware in QEMU.

## Modes

| Mode | When | What runs |
|---|---|---|
| `normal` | The configuration is valid | All |
| `recovery` | The configuration is rejected, or a factory reset is not complete, or safe mode: 3 crashes in sequence, or a restart into safe mode on request | The network, the web UI, the shell, the access point and the firmware upload. No polling, no MQTT |
| `commissioning` | After a factory reset: no configuration | The network (DHCP), the web UI, the shell and the access point |
| `fault` | The boot did not find the PSRAM | The network, the web server, the shell and the access point. The gateway does not start |

`GET /api/status` (`mode`), the card **Gateway** of the web UI and the shell command `status` give the mode.

**Safe mode.** Safe mode starts after 3 crashes in sequence, or on request: the button **Restart into safe mode** of the web UI, `POST /api/restart?safe=1` or the shell command `restart safe`. Safe mode ends after 60 min, or at the next restart. The mode is `recovery`. `GET /api/status` gives `safe_mode` with its cause (`request` or `crashes`) and the time left, and the web UI shows a banner of its own. For the procedure, refer to [Restart into safe mode or do a factory reset](../how-to/restart-into-safe-mode-or-reset.md).

![The card Gateway of the Status tab: the firmware, the reset reason, the uptime, the mode, the configuration, the free internal heap, the largest free internal block, the free PSRAM and the chip temperature (46.5 °C). Below the values: the button Identify and the text 'For 10 s, the status LED blinks and the buzzer beeps.'](../images/status-gateway-card.png)

The card **Gateway** of the Status tab also gives the health of the gateway:

- the free internal heap and its minimum;
- the largest free internal block;
- the free PSRAM;
- the temperature of the chip.

Without a reading of the temperature sensor (for example in QEMU), the temperature is "not available". The NBIRTH and the diagnostics give the same values. Refer to [The Node metrics](sparkplug-messages-and-metrics.md).

## Status LED

The LED shows the pattern of the highest state in the table.

| State | Pattern |
|---|---|
| Factory reset in progress, also at a boot that completes a factory reset after a power cut | White, continuous |
| BOOT held for more than 10 s: a release starts the factory reset | White, continuous |
| BOOT held for 2 to 10 s: a release starts the access point | White, blinks at 4 Hz |
| Identify (10 s) | Red, green and blue in turn, each color 250 ms (4 Hz) |
| Claim window open, or access point open (no key) | Blue and white, alternately (each color 250 ms) |
| Commissioning | Blue, blinks at 1 Hz |
| Safe mode (on request or after 3 crashes) | Blue and amber, alternately (1 Hz) |
| Configuration not valid (recovery mode, but not safe mode), or the PSRAM fault mode | Red, blinks at 4 Hz |
| No network (no IP address) | Red, continuous |
| No valid clock: the MQTT session waits | Magenta, blinks at 1 Hz |
| No broker | Amber, blinks at 1 Hz |
| A Modbus device is offline (a disabled device does not count) | Green and orange, alternately (1 Hz) |
| Normal operation | Green, continuous |

While the access point runs with WPA2, a short cyan flash (125 ms) each 2 s replaces the pattern, from "Commissioning" to "Normal operation". The log gives the name of the pattern, for example `led: status: operating, access point`.

**Identify.** The button **Identify** of the card **Gateway**, `POST /api/identify` and the shell command `identify` start the identify for 10 s. Use it to find the gateway in a panel with a number of gateways. A second request during the 10 s does not start the time again: the web UI keeps the button disabled, and the API gives `409`. The identify has a higher priority than all the patterns of the gateway. Only the patterns of the BOOT button and of a factory reset have a higher priority, because the person at the panel needs them.

## Buzzer

One beep is 150 ms of tone and 150 ms of silence.

| Signal | When |
|---|---|
| 1 beep | BOOT held for 2 s, after the end of the boot. Release the button now to start the access point. During the boot, the gateway gives no beep. The log gives `BOOT held 2 s, no beep: still starting, a release now does nothing` |
| 1 beep | You release BOOT after 2 to 10 s, after the end of the boot: the access point starts (and the claim window opens, if no web password is set). During the boot, the gateway ignores the release and gives no beep. The log gives `BOOT gesture ignored: still starting (no beep)` |
| 1 beep | The claim window set the first web password |
| 2 beeps | BOOT held for 10 s: a warning. Release the button now to start the factory reset. While you hold the button, the gateway erases nothing |
| 2 beeps | You release BOOT after 10 s, or a factory reset from the web UI, the REST API or the shell: the factory reset starts |
| 1 short beep each 0.5 s, for 10 s | Identify. The beep is 150 ms of tone. The alarms and the other beeps wait until the end |
| 1 beep each 10 s | Alarm `device_offline`: one or more Modbus devices are offline. A disabled device does not count |
| 2 beeps each 10 s | Alarm `mqtt_down`: no Sparkplug session |
| 3 beeps each 10 s | Alarm `config_error`: the configuration is rejected. Safe mode does not sound this alarm, because it does not reject the configuration |

The alarms sound only when `local_io.buzzer.alarms` contains them. `mqtt_down` and `device_offline` sound after `local_io.buzzer.alarm_after_s` (30 s by default). `config_error` sounds immediately. If a number of alarms are active, the alarm with the most beeps sounds.

## BOOT button

The firmware reads the button only after the boot. It must see the button released one time.

The gateway gives a signal at 2 s and at 10 s while you hold the button. The action starts when you release the button (#292).

| Push | While you hold the button | At the release |
|---|---|---|
| Less than 2 s | Nothing | Nothing |
| 2 to 10 s | At 2 s: 1 beep, and the LED blinks white | 1 beep. The access point starts, or its times start again. Without a web password, the claim window also opens |
| More than 10 s | At 10 s: 2 beeps, and the LED is white | 2 beeps. Factory reset: the gateway erases the configuration, the secrets and the commissioning password. Then it restarts in commissioning mode |
| Held at power-on | The ROM starts the download mode for a flash over USB. The firmware does not start | — |

The web UI, `POST /api/factory-reset?confirm=1` and the shell command `factory-reset` do the same factory reset as the button.

During the boot, the gateway gives no beep at 2 s and ignores a release between 2 and 10 s. The factory reset operates also during the boot.

The gateway has no time limit for a push. After 10 s, the factory reset starts at the release, also after a push of some minutes. To stop a factory reset after the 2 beeps, disconnect the power while you hold the button. Then release the button, and connect the power again.

A button that stays down, for example a defective button, gives the 2 beeps at 10 s, then nothing more. The gateway continues its work, and the LED stays white. The factory reset starts only when the button comes up.

For the access point, refer to [The access point](access-point.md). For the clock signals, refer to [The clock and the RTC battery](clock-and-rtc-battery.md).
