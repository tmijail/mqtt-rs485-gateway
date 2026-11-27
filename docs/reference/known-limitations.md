# Known limitations

This document lists the known limits of the firmware, by subsystem. Each item is short, and gives the issue and the document with the detail.

- For the status of each requirement, refer to [Requirements status](requirements-status.md).
- For the choices where the Sparkplug specification permits a choice, refer to [Sparkplug behavior](sparkplug-behavior.md).

> **Warning: Do not use this document as verified data for the board.** The firmware has never run on the board (#1, #18). The firmware ran only in host tests and in QEMU.

## No test on the board (#1, #18)

These parts never ran:

- the W5500 driver and its SPI;
- the RS485 hardware mode, with the RTS/DE timing at 115200 baud. The UART ISR is in IRAM (`CONFIG_UART_ISR_IN_IRAM`), so DE releases in time also when the cache is disabled. Nobody examined this with an oscilloscope;
- the real TCA9554, and the real polarity of the DIs and the DOs;
- the color order of the LED;
- the real octal PSRAM, and the fault mode with a defective PSRAM. QEMU runs its own PSRAM model (#85, #135);
- the boot with QIO flash;
- the Wi-Fi: association, reconnection, the switch between two real links, and the binding of the MQTT socket (`SO_BINDTODEVICE`);
- the SDMMC driver, FAT and the format of the microSD card (#131);
- the PCNT units, the RTC, the buzzer and the BOOT button.

The QEMU build is different from the board build (`sdkconfig.qemu`). For the board data to verify, refer to [Board data to verify](board-data-to-verify.md). For the test environment, refer to [Tests](tests.md).

## The install tool of the site (#219)

The page [Flash the firmware from the browser](../how-to/flash-the-firmware-from-the-browser.md) has these limits:

- Nobody flashed a board with it. The unit tests examine its logic without a browser and without a board. The first flash of a board is part of #18.
- The USB-C connector goes to the USB-Serial-JTAG peripheral of the ESP32-S3 (VID:PID 303A:1001). The board has no USB-UART bridge (#18).
- On the first board, `esptool` started the download mode through the USB, without the BOOT button. Its reset did not remove the port from the computer. The RESET button removes the port for a short time. The page itself did not flash a board yet.
- The times of the reader (5 s without the start log, 30 s with it) come from the prototype and from QEMU. Measure them on the board.
- The flash uses 460800 baud after the stub of esptool-js starts. A USB chip that cannot use this rate stops the flash. Then use esptool with a lower rate.
- Only Chrome and Edge on a computer have Web Serial. The other browsers get the image files and the esptool command.
- A flash over USB has no pre-flight check, no trial and no rollback. The update over USB with these protections is #281.
- The site has a copy of each release. A damaged release on GitHub stops the publication of the site (refer to [The copy on the documentation site](release-manifest.md#the-copy-on-the-documentation-site)).

## The shell on the USB console (#293)

- The USB-Serial-JTAG driver of ESP-IDF drops console output when the computer does not read for 50 ms. A long answer of the shell, for example `config show` or `help`, can thus lose lines. The answer does not show the loss.
- `coredump dump` is protected: it waits for the computer, and it gives the size, the number of lines and the SHA-256 of the dump. Refer to [The core dump](../explanation/core-dump.md#the-copy-at-the-console).
- If the computer does not read for 3 s, `coredump dump` stops without the END marker. Use `GET /api/coredump` when a network is available.
- Nobody examined the wait for the computer on the board yet. QEMU has no USB console.

## Memory (REL-2)

- The code of the project is static, and does not allocate in the poll, encode and publish path. But lwIP and esp-mqtt allocate from the heap. Their use is bounded, not static.
- A TLS session uses approximately 40 KB of mbedTLS heap, in PSRAM. Without flash encryption, these buffers are in plain text outside the chip.
- The Modbus TCP server uses 5.8 KB of `.bss` (a stack of 4 KB and the buffers), also when it is off.

For the memory budget, refer to [Architecture](architecture.md#memory-rel-2).

## The clock (TIME-1, TIME-2)

- Without a valid clock, the gateway opens no MQTT session (#160). Without the RTC battery and without SNTP, the gateway sends nothing after a power cut until an operator sets the time.
- The RTC time and the browser time are valid, but not synchronized. The RTC drifts some seconds per day until SNTP corrects it.
- A first SNTP server that does not answer delays the first time by approximately 15 s.
- The real I²C access to the RTC is compile-only.

For the detail, refer to [The clock and the RTC battery](clock-and-rtc-battery.md).

## The MQTT session

- A slow broker blocks the `sparkplug` task during each publish. Each socket write waits up to 5 s (`network.timeout_ms`). The memory does not increase.
- During a session, the gateway publishes the last value of each device in each pass. Store & forward keeps only the data without a session.
- Each publish has a budget of 8 s (`CONFIG_GW_MQTT_PUBLISH_BUDGET_MS`, #59). Then a timer shuts down the socket, and esp-mqtt closes the connection.
- Thus one publish takes 13 s at most (the budget and one socket timeout), less than the 15 s of the task watchdog.
- A link that cannot send the largest DBIRTH in the budget cannot keep a Sparkplug session. The gateway stays without a session, but does not restart. The QEMU build uses a budget of 2 s.
- `esp_mqtt_client_destroy` can block the `sparkplug` task for the network timeout (5 s) during a connection attempt. This stays below the watchdog.
- When the old interface is dead, the explicit NDEATH does not arrive. The broker publishes the Will after 1.5 × `keepalive_s`, possibly after the new NBIRTH. A host discards it by bdSeq.
- A message larger than the Maximum Packet Size of the broker does not go out (#73). Refer to [Sparkplug behavior](sparkplug-behavior.md#the-maximum-packet-size-of-the-broker-73-76).

## Commands

- The gateway discards a command of more than 1024 bytes, and a command that esp-mqtt fragments, with a log line.
- The gateway discards all of an NCMD or a DCMD that nanopb cannot decode fully. Examples: a name of more than 63 characters, or a string of more than 256 characters. The log gives `string overflow` and the limits.
- The write queue of the bus has 8 positions (`CONFIG_GW_BUS_WRITE_QUEUE_LEN`). If all the writes of a DCMD do not fit, the gateway discards all of the DCMD (#60).
- The gateway discards an unknown metric, a read-only metric and a value out of the write limits alone (#196). The other metrics of the DCMD apply.
- A write is retried as any transaction. A slave that executes the write and does not respond receives it two times.
- A write of a bit or a field of bits (#202) reads the register and writes all of it again. If the slave changes another bit of that register in that time, the change is lost. The gateway does not use FC22.

For the rules of a DCMD, refer to [Sparkplug messages and metrics](sparkplug-messages-and-metrics.md#dcmd).

## Sparkplug

- The aliases depend on the order of the tags in the configuration. A new order gives new aliases in the next births.
- Templates have no version and no nesting. No test used a real host such as Ignition (SPB-18).
- A template parameter is String, Int64, Double or Boolean, and a device type has 8 parameters at most (#276). The gateway does not read a parameter from the bus, and a DCMD cannot write it.
- A change of the parameters of a type needs a manual step in Ignition: MQTT Engine does not update a UDT definition that it already has. A parameter name with a hyphen or a space can fail in a `{Name}` reference of a UDT. Refer to [Notes for Ignition](sparkplug-behavior.md#notes-for-ignition).
- No document says how MQTT Engine reads a Boolean parameter, or what it does with a parameter that an instance does not give (#276).
- The Sparkplug TCK examines the template parameter assertions only when a device type has a parameter. Refer to [The template parameter assertions](sparkplug-behavior.md#the-template-parameter-assertions-251-276).
- Bytes and File hold 64 B at most. File has no metadata, and there is no file transfer (SPB-19).
- A text of a metric has 256 characters at most (`SPB_STRING_MAX`). A text of a property or of a DataSet cell has 63 characters at most (#87). The firmware does not truncate a longer text: it rejects the message, and the log gives the cause.
- The bdSeq is in RAM only, and starts at 0 at each boot (#161).

For the detail, refer to [Sparkplug behavior](sparkplug-behavior.md).

## Store & forward and the microSD card (SPB-14)

- An unplanned restart loses the RAM buffer. Only the samples on the card stay.
- The history uses QoS 0. A power cut can lose the messages that TCP did not send yet.
- A restart during a replay can send 64 KB of samples again (128 KB if the power cut damages the save). The host must remove the duplicates.
- GPIO45, the D0 pin of the card, is a strapping pin, and the board has a pull-up on it. On the first board, an eFuse sets the voltage of the flash, and the ROM ignores GPIO45. The spill does not change the level of GPIO45. The spill is off by default. Refer to [The microSD card and GPIO45](store-and-forward.md#the-microsd-card-and-gpio45).
- The card is only for the gateway. An exFAT card, a GPT card and a card without a file system do not mount.
- The card, its format and its power cut behaviour have no test on the board (#131).

For the detail, refer to [Store & forward](store-and-forward.md).

## TLS (MQTT-4, MQTT-5)

- TLS needs a valid clock to check the certificate dates. With `CONFIG_MBEDTLS_HAVE_TIME_DATE=n`, the gateway checks no dates, also no expired certificates. The connection still waits for a valid clock.
- The certificate files are in LittleFS, next to `config.json`. A change of a file applies at the next restart.
- The private key of the client is in plain text in LittleFS, and in RAM while the firmware runs. Anyone who reads the flash gets the key.
- The key cannot go to NVS, because esp-mqtt needs all of the key in memory. The gateway does not accept a key with a password (encrypted PEM).
- The DS peripheral keeps a key out of reach of the firmware, but it burns an eFuse. It is out of scope (#133). Refer to [No eFuse features](../explanation/no-efuse-features.md).
- The public CA bundle is the common subset of ESP-IDF (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN`). It is compile-only: the E2E uses its own CA.
- With TLS 1.3, esp-tls does not give the cause of a certificate rejection. The firmware uses its own verification callback, and the log gives the cause (#70). The upstream report is #237.
- The firmware chains to the callback of the CA bundle through the public API `esp_crt_bundle_attach()` (#77). If a new ESP-IDF changes the callback type, the build fails. The gateway never accepts a certificate without verification.

For the procedure, refer to [Connect to a broker with TLS](../how-to/connect-to-a-broker-with-tls.md).

## The list of brokers (MQTT-6)

- With `primary_return_s` > 0, each return to the primary broker cuts the session for a short time. This occurs also when the primary broker is still down. The gateway cannot probe a broker without a cut.
- With `primary_return_s: 0` (the default), the gateway keeps a backup broker that operates.
- Each broker change is a new session (NDEATH, NBIRTH, all the DBIRTHs). The hosts see it.
- The credentials, the `client_id`, TLS and the CA are the same for all the brokers.
- The brokers share no state. A command in transit on the old broker is lost. A primary host must connect to all the brokers.
- A host name with more DNS records is one broker. esp-mqtt uses the first address.

## MQTT 5 (MQTT-8)

- `CONFIG_MQTT_PROTOCOL_5` adds code to the image, also when the configuration uses 3.1.1. Without this option, the gateway reports a configuration with `"5"` and does not connect.
- The gateway uses only the MQTT 5 functions that Sparkplug needs. Refer to [Sparkplug behavior](sparkplug-behavior.md#mqtt-5-mqtt-8-155).

## The network (NET-4, NET-6, NET-7)

- The gateway finds a broker that it cannot reach from its own attempts, not with a probe (#57). With one broker and the default values, the failover takes approximately 20 to 30 s.
- The gateway does not probe the failed interface. The session goes back to it after a wait of 60 s, doubled up to 30 min. These are host tests only: QEMU has no Wi-Fi.
- esp-mqtt 1.1.0 leaks approximately 17 B for each `esp_mqtt_set_config()` call with `network.if_name` or `key_password` (#58). A wrapper refuses these fields, and the host test `check_set_config` finds a direct call.
- Remove the wrapper when esp-mqtt corrects the leak. The upstream report is #236. Nobody sent it yet.
- If Wi-Fi gets an address before Ethernet at boot, the first session uses Wi-Fi. The session goes back to Ethernet after `failback_s`: one extra session.
- The Wi-Fi reconnection uses a backoff with jitter from 1 to 30 s. The radio power save is off (`WIFI_PS_NONE`), for the latency of the commands and for the entropy source (#267).
- The radio is on at each boot, also without `network.wifi`, because it is the entropy source of the random values (#267). Refer to [Commissioning and secrets](commissioning-and-secrets.md#the-random-values). It uses more power, and the chip is warmer. The Wi-Fi driver always uses its internal RAM. No configuration key turns the radio off. If a site does not permit a radio, open a new issue.
- The station without a connection is compile-only. Nobody examined on the board that it transmits no frames and that the radio does not go into a sleep mode (#130).
- The Wi-Fi driver keeps no credentials in its NVS (`WIFI_STORAGE_RAM`). The password is in the secrets in NVS (CFG-7).
- The scan of the Wi-Fi networks is compile-only on the radio (#208, #130). A scan while the station tries to connect can fail (`503`).
- With two interfaces on the same subnet, lwIP selects the interface for SNTP and DNS. Only MQTT is bound to the active interface.
- MQTT over IPv6 has no end-to-end test: the test container has no IPv6. SNTP and DNS over IPv6 are compile-only.
- IPv6 has no DHCPv6 and no static router: the router comes from the RAs. Without RAs, a static address reaches only its /64. The static address is only for Ethernet.
- The SLAAC address uses EUI-64, so it shows the MAC. lwIP has no temporary addresses, and does not tell when an address expires.
- The mDNS instance name is `<group_id>/<edge_node_id>`, cut at 63 bytes at the boundary of a UTF-8 character.
- The mDNS responses are compile-only (NET-6). Refer to [Commissioning and secrets](commissioning-and-secrets.md).

## The captive portal of the access point (NET-5, #274)

- Nobody examined the captive portal with a real phone (#130). The E2E examines the DNS answers, the redirects and the guide page over the emulated Ethernet. The QEMU build has no radio.
- The captive network sheet of iOS and macOS cannot open Safari. There, the button "Open in the browser" copies the address to the clipboard. The operator then opens Safari and pastes the address.
- The window of Android can open the address in the same window. That window does not accept the self-signed certificate of the gateway. When the window shows a certificate error, the address is also in the clipboard. Nobody examined this on a phone (#130).
- The HTTPS port has no captive portal. A phone that sends an HTTPS probe gets the certificate of the gateway, and each probe costs a TLS handshake of the web server.
- After the guide page, the phone tells that the network has no Internet. Some phones then send the traffic of the browser to the mobile data, and the browser does not get to `192.168.4.1`. The procedure tells the operator to stay on the network.
- The DNS server answers each name with the address of the access point, with a TTL of 10 s. A phone that leaves the access point can use these answers for 10 s more.
- A DNS query in more than one IP fragment gets no answer. A DNS query of a phone is one small datagram.

## Modbus (MB-6, MB-15)

- A `string` tag holds 32 characters at most (16 registers), printable ASCII only. Report by exception compares a 64-bit FNV-1a hash of the text.
- With `scale` or `offset`, an `int64` or `uint64` tag becomes a Double, and is not exact above 2^53.
- For the datatypes, refer to [The datatypes in Sparkplug](configuration.md#the-datatypes-in-sparkplug).
- **Bias of the RS485 bus.** The board has no bias resistors on A and B (#18). Without a bias at one point of the bus, an idle bus with terminations floats, and the gateway can count frame errors. Refer to [Add bias, if necessary](../how-to/connect-the-rs485-bus.md#add-bias-if-necessary).

### Modbus TCP (MB-15)

- **No authentication and no encryption.** A client in `allowed_clients` can read and write each register of each slave on the bus. Thus the server is off by default.
- `allowed_clients` filters by source address. A device on the same network can use a false address.
- The gateway always refuses a connection through the access point (#120). While the access point runs, it also refuses a client in `192.168.4.0/24`, also a plant client.
- The server listens on IPv4 only.
- The server accepts 2 clients at the same time (`CONFIG_GW_MBTCP_MAX_CLIENTS`). It closes a further connection without a response.
- One transaction runs at a time for all the clients. After 8 s without the bus (`CONFIG_GW_MBTCP_RESPONSE_WAIT_MS`), the client gets the exception 0B.
- The gateway answers FC07, FC08, FC17, FC22, FC23 and FC43 with the exception 01. There is no broadcast (unit 0).
- The counters of the connections, the forwarded requests and the exceptions of the gateway are only in the shell (`devices`). The counters of each unit are also in `Node Info/Modbus TCP`.

For the keys, refer to [`modbus_tcp`](configuration.md#modbus_tcp).

## Remote syslog (REL-4)

- There are no logs over MQTT. They would put traffic that is not Sparkplug into the session. Syslog operates also when the broker is down.
- Syslog uses UDP, with no acknowledgment. The gateway does not send a lost datagram again. A gap in `sequenceId` on the server is a loss on the network.
- The gateway discards a line when a different task formats a line at the same time, and when the ring is full. The server then gets `N log line(s) not forwarded`.
- The gateway cuts a line at 256 bytes, and the message at 199 characters.
- The lines from before the configuration loads, `ESP_EARLY_LOG` and the lines from interrupts stay only on the console.
- There is no TLS (RFC 5425). If the name of the server does not resolve, the gateway tries again each 5 s.

For the keys, refer to [`syslog`](configuration.md#syslog).

## Local I/O (IO-1 to IO-7)

- The gateway loses a DI change that is faster than the debounce of that DI. With 20 ms, a counter sees approximately 25 Hz at most.
- **Fast counters (#218).** Up to 4 DIs use one PCNT unit each. The glitch filter is 0 to 12 787 ns (1 023 APB cycles). The `local_io` task reads each unit at least each 250 ms.
- Thus a fast counter loses no pulse up to approximately 130 kHz. The input circuit of the board sets the real limit (not measured, #126). QEMU has no PCNT.
- **Pulse DOs (#210).** The accuracy of a pulse is the wake of the `local_io` task, some milliseconds, not measured. A pulse DO is OFF after a restart, also with `hold`.
- **Run hours (#217).** An NDATA gives a new value when the channel changes its state, and each 60 s while it stays on. A DO that the expander does not read counts as off.
- **Saved counts.** The counters and the run hours go to NVS each `counter_save_s` (600 s by default), and at a controlled restart. A power cut, a panic or the watchdog loses the counts since the last save.
- With `counter_save_s` 60, there are approximately 1 440 writes of 192 B per day: decades with the wear leveling of NVS. There is no NCMD to set the counters to 0.
- The NDATA timestamp is the time of the batch of changes, not of each bit.
- **DO boot state (IO-3).** At each boot, before the configuration loads, the DOs go to OFF. The `hold` DOs of the last valid configuration keep their state (a mask in NVS).
- The TCA9554 does not reset with the ESP32. The firmware reads its latch, writes it again, and sets its polarity register to 0 before it sets the pins to output.
- A change of `hold` applies from the next boot. The first step of a boot uses the mask of the earlier configuration.
- **Safe state on MQTT loss.** The Sparkplug session counts, not the link. The safe state applies one time for each cut. When the session comes back, the DOs do not go back to their earlier state.
- **No answer from the expander.** The DOs are null in the NBIRTH, and the gateway refuses the NCMDs on them. The `local_io` task tries again at 1, 2, 4 … up to 60 s, also in recovery mode and in commissioning (#74).
- **Local rules (IO-7).** The only rules are DO = DI, DO = not DI, and toggle on an edge. There are no timers, no combinations of DIs and no rules for the Modbus slaves.
- A DO with a rule refuses the NCMD and has no safe state on MQTT loss. When the expander is down, the rules wait.
- The polarity of the DOs (active low) comes from the demo firmware on the first board and from ESPHome. It is not measured yet (#1). If a measurement shows the opposite, set `CONFIG_GW_DO_ACTIVE_LOW=n`.
- **The current of the DOs.** One output can sink 500 mA. The eight outputs share one ULN2803A, and its total power must stay low. Refer to [The limit of the DO current](#the-limit-of-the-do-current).
- The ULN2803A has no protection against a short circuit or a high temperature. A short circuit of a load can destroy its output. The firmware reads the latch of the expander, not the real state of an output.
- **The current of the DIs.** A closed contact gets less than 1 mA from DICOM. A relay contact with oxide on it can then give random changes. Use contacts for low currents (for example with gold plating) or electronic outputs.
- **The expansion terminals.** `SDA` and `SCL` are the I²C bus of the TCA9554 and of the RTC. Do not connect field cables or other devices to them: a fault on this bus stops the control of the DOs. `IO0` is probably GPIO0, the strapping pin of the BOOT button. Do not connect it to field cables: a low level at power-on starts the download mode.

For the keys, refer to [`local_io`](configuration.md#local_io). For the procedure, refer to [Set up the local inputs and outputs](../how-to/set-up-the-local-io.md).

### The limit of the DO current

The ULN2803A is a Darlington array. An active output has a voltage of 1.1 V at 100 mA, 1.3 V at 200 mA and 1.6 V at 350 mA (maximum values of the TI data sheet SLRS049C). Each active output makes heat in the same package.

- The power of the package is the sum, for each active output, of its voltage multiplied by its current.
- The data sheet of the ULN2803A gives the thermal resistance of the SOIC package as "TBD". The data sheet of the ULN2803C (SLRS076B), the same family in a similar SOIC package, gives approximately 75 °C/W, and recommends a junction temperature below 125 °C.
- At an ambient temperature of 50 °C, the package can thus have approximately 1 W.
- With the eight outputs on, approximately 1 W is approximately 100 mA for each output.
- One output alone can have its 500 mA.

For larger loads, use interposing relays. The temperature measurement of #126 (L5) confirms or corrects this limit.

## The LED, the buzzer and the diagnostics

- **Buzzer (IO-6).** The buzzer uses LEDC at 2.7 kHz. Nobody examined the buzzer type or its resonance frequency. The demo uses 1 kHz.
- You cannot silence an alarm while its condition continues.
- The `config_error` alarm uses the setting of the last valid configuration (NVS). A gateway that never had a valid configuration gives no `config_error` alarm.
- The alarms and the other beeps wait until the end of an identify (10 s, #207).
- **The temperature of the chip (#212).** The internal sensor has a range of 20 to 100 °C, with an error below 2 °C (data sheet). The chip is warmer than the air of the panel.
- QEMU has no model of the sensor: `Node Info/Temperature` is null. The board test #129 must examine the value.
- **The live log (#200).** The ring of 16 KB holds approximately 150 to 250 lines of `esp_log`.
- The live log does not have the lines from an interrupt, or the text that the firmware writes with `printf`, for example the commissioning password.
- A line that comes while the web server copies the ring is lost (`dropped`).

For the signals, refer to [The status signals](status-signals.md). For the live log, refer to [Read the live log](../how-to/read-the-live-log.md).

## QEMU and restarts

- In QEMU, `esp_restart` does not reset UART1 or the interrupt matrix. A Modbus response during the restart can hang the next boot in `uart_driver_install`.
- Thus each controlled restart makes the bus silent first (`gw_bus_quiesce`): REST, shell, NCMD Reboot and factory reset. On the board, this also prevents a cut frame.
