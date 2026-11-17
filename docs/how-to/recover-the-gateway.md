# Recover a gateway

This document tells what to do when a gateway does not operate normally: the configuration is rejected, the gateway restarts again and again, nobody knows the web password, or the web UI is not available.

- For the meaning of each LED pattern, refer to [The status signals](../reference/status-signals.md).
- For the details of the modes, refer to [`idf/README.md` §3.4 to §3.6](../../idf/README.md#34-configuration).

> **Warning: Do not use this document as verified data for the board.** The firmware has never run on the board (#1, #18). The E2E tests these modes in QEMU.

## Find the mode of the gateway

| LED | Mode | Go to |
|---|---|---|
| Red, blinks fast | Recovery: the configuration is not valid | [The configuration is rejected](#the-configuration-is-rejected) |
| Blue, blinks slowly | Commissioning: no configuration (after a factory reset) | [The gateway has no configuration](#the-gateway-has-no-configuration) |
| Red, continuous | No network address | [The gateway has no network](#the-gateway-has-no-network) |
| Magenta, blinks slowly | No valid clock: the MQTT session waits | [The clock and the RTC battery](../reference/clock-and-rtc-battery.md) |
| Amber, blinks slowly | No broker | [The gateway does not connect to the broker](#the-gateway-does-not-connect-to-the-broker) |
| Green and orange, alternately | A Modbus device is offline | [Connect the RS485 bus](connect-the-rs485-bus.md), "If there is a problem" |

The web UI shows the mode in the card **Gateway**, and a red banner gives the cause. The shell command `status` gives the same data.

## The configuration is rejected

In recovery mode, the polling and MQTT do not start. The network, the web UI, the shell and the access point start.

1. Open the web UI. The gateway uses the network of the last valid configuration, or DHCP on Ethernet. You can also use the access point: refer to [Get on the access point on a later visit](get-on-the-access-point.md).
2. Read the cause in the red banner. It gives the JSON path, or the line and the column.
3. Correct the configuration. Refer to [Change, back up and restore the configuration](change-the-configuration.md).
4. Click **Validate**, then click **Save and restart**.

After a firmware update into recovery mode, the card **Firmware** can show the button **Restore the configuration from before the update**. Click it to load the document that the earlier image used. In the confirmation, click **Restore and restart**. The gateway then restarts with that document.

## The gateway restarts again and again

After 3 crashes in sequence, the gateway starts in safe mode. Safe mode is recovery mode with the cause "repeated crashes". Modbus and Sparkplug do not run. The web UI, the shell and the firmware upload run.

1. Open the web UI. Read the reset reason in the card **Gateway**.
2. If the card **Gateway** shows a core dump, download it. Refer to [Get and decode a core dump](decode-a-core-dump.md).
3. If the crashes started after a configuration change, correct the configuration and save it.
4. If the crashes started after a firmware update, go back to the earlier image. Refer to [Update the firmware](update-the-firmware.md).
5. Report the crash with the core dump in an issue of the project.

Safe mode ends after 60 min with a restart. A restart from the web UI or a power cycle also ends it.

## The gateway has no configuration

After a factory reset, the gateway is in commissioning mode. It has no configuration, no web password and a new commissioning password.

1. Set the first web password. Refer to [Set the first web password on site](set-the-first-web-password.md).
2. Load the configuration. Refer to [Change, back up and restore the configuration](change-the-configuration.md), "On a new gateway, or after a factory reset".

## Nobody knows the web password

### With the USB console

The other settings stay.

1. Connect a computer to the USB-C connector. Open a serial terminal at 115200 baud.
2. Type `config show`. Copy the document. The secrets show as `********`.
3. In the copy, replace the value of `web.password` with a new password (8 to 63 printable ASCII characters).
4. Type `config load`, then paste the document.
5. Make sure that the gateway restarts, and that the web UI accepts the new password.

The new password is also the Wi-Fi password of the access point.

### Without the USB console

Do a factory reset. A factory reset erases the configuration and all the secrets. Restore the configuration from a backup after the reset.

## Do a factory reset

> **Caution: Back up the configuration first, if you can.** The factory reset erases the configuration, the secrets, the web certificate and the commissioning password.

1. Make sure that the gateway completed its boot.
2. Push the BOOT button, and hold it.
3. At 2 s, the LED blinks white. Continue to hold the button.
4. At 10 s, the gateway gives two beeps, and the LED becomes white. Release the button.
5. Wait for the restart. The LED blinks blue: commissioning mode.
6. Do the steps of [The gateway has no configuration](#the-gateway-has-no-configuration).

Do not push BOOT when you connect the power. GPIO0 is a strapping pin: the ROM then starts the download mode, and the firmware does not start.

## The gateway has no network

1. In the card **Network and MQTT**, read the state of each interface: `no link`, `no address` or `up`.
2. For `no link`, examine the cable and the port of the switch.
3. If the configuration uses a static address, make sure that it is correct for the network.
4. Look for a DHCP lease of the gateway in the DHCP server of the network.
5. If the Ethernet does not operate, use the access point. Push BOOT for 2 to 10 s. Refer to [Get on the access point on a later visit](get-on-the-access-point.md).

## The gateway does not connect to the broker

1. Read the log of the gateway on the console or on the syslog server. It gives the cause of each failed connection.
2. Make sure that the broker address and the port are correct.
3. Make sure that the gateway can reach the broker: type `ping <broker address>` in the shell.
4. With TLS, make sure that the clock is valid and that the CA of the broker is correct.
5. With `sparkplug.primary_host_id`, make sure that the primary host is online. The gateway sends no NBIRTH until the host publishes its STATE `online`.

## The web interface is off

With `web.enabled: false`, the gateway starts no web server in normal mode. MQTT, Modbus and the other services continue. The BOOT button starts the web server again until the next restart.

1. Push the BOOT button of the gateway for 2 to 10 s, until you hear 1 beep. Release the button.
2. Make sure that the LED blinks white during the push. The web server and the access point start.
3. Open the web UI on the plant network, or on the access point. Refer to [Get on the access point on a later visit](get-on-the-access-point.md).
4. In the Configuration tab, go to the card **Web interface**.
5. Turn on **Enabled**.
6. Click **Save and restart**.

After the restart, the web server starts at each boot. Without step 5, the web server stops again at the next restart.

You can also set `"enabled": true` in `web` with the USB console. The steps are the same as in [With the USB console](#with-the-usb-console): `config show`, change the document, then `config load`.

## The web UI is not available in any mode

Flash the firmware over USB. The owner of the board always keeps this access: the project burns no eFuses.

1. Connect a computer to the USB-C connector.
2. In `idf/`, type `idf.py -p <port> flash`.
3. If the flash does not start, disconnect all power. Hold BOOT, connect the USB cable, and release BOOT. The ROM then starts the download mode.

A flash with `idf.py flash` also writes the `fs/` folder into the LittleFS partition. Without a `config.json` in that folder, the gateway uses the embedded example configuration. After a factory reset, it stays in commissioning mode. The NVS (secrets, web password) stays. To erase all, type `idf.py -p <port> erase-flash` before the flash.

The PSRAM fault mode is a different case: the boot did not find the PSRAM. Only the network, the web server and the shell start. Examine the board. The firmware does not operate without PSRAM.
