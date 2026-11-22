# Requirements status

This document gives the status of each requirement ID of [the product specification](../SPEC.md), with the evidence and the main limit. Each row links to the document with the detail.

- For the known limits by subsystem, refer to [Known limitations](known-limitations.md).
- For the tests, refer to [Tests](tests.md).

> **Warning: Do not use this document as verified data for the board.** The firmware has never run on the board (#1, #18). "Done" means that host tests or the E2E in QEMU examine the function, not the board.

## Legend

| Status | Meaning |
|---|---|
| done | The firmware has the function. Host tests or the E2E in QEMU examine its main part |
| compile-only | The firmware has the function, but its main part needs hardware that QEMU does not have |
| partial | The firmware has a part of the function only |
| not done | The firmware does not have the function |
| out of scope | A decision of the product owner removed the function |

| Evidence | Meaning |
|---|---|
| H | Host test (`idf/test/host/`) |
| Q | E2E with the real firmware in QEMU |
| P | Decode with the official Eclipse Tahu proto (`idf/tools/spb_decode.py`) |
| C | Compiled only, never run |

The column MVP has ✅ for the IDs of the MVP (SPEC §5).

## Network (NET)

| ID | MVP | Status | Evidence and limits | Detail |
|---|---|---|---|---|
| NET-1 | ✅ | done | Q: the `esp_netif` and event path, with openeth. C: the W5500 driver and its SPI | [Architecture](architecture.md) |
| NET-2 | ✅ | done | Q: DHCP. C: the static IP and the manual DNS | [`network`](configuration.md#network) |
| NET-3 | ✅ | compile-only | C: the E2E uses an IP literal, not DNS | [`mqtt`](configuration.md#mqtt) |
| NET-4 | | compile-only | H: the interface selection, failback and "no service" (#57). Q: a Wi-Fi configuration without a radio. C: the Wi-Fi driver and the switch | [Known limitations](known-limitations.md#the-network-net-4-net-6-net-7) |
| NET-5 | | done | H: the WPA2 PMK, the stops, and the DNS answer, the probe paths and the guide page of the captive portal (#274). Q: the simulated access point, and the captive portal over the emulated Ethernet. C: the SoftAP, and the captive portal window of a real phone (board test #130) | [The access point](access-point.md) |
| NET-6 | | done | H + Q: the hostname, and the start of the mDNS responder. C: the mDNS responses | [Commissioning and secrets](commissioning-and-secrets.md) |
| NET-7 | | done | H + Q: link-local, SLAAC, static address, ping. No test: MQTT over IPv6. C: SNTP and DNS over IPv6 | [Known limitations](known-limitations.md#the-network-net-4-net-6-net-7) |

## Time (TIME)

| ID | MVP | Status | Evidence and limits | Detail |
|---|---|---|---|---|
| TIME-1 | ✅ | done | Q: the UTC timestamps are less than 30 ms from the host time. No MQTT session without a valid clock (#160) | [The clock and the RTC battery](clock-and-rtc-battery.md) |
| TIME-2 | | done | H: the RTC data. Q: a simulated PCF85063. C: the real I²C. The RTC battery is necessary (#160) | [The clock and the RTC battery](clock-and-rtc-battery.md) |
| TIME-3 | | done | H + Q: `Node Info/Time Synced`. Only SNTP sets it to `true` | [Sparkplug messages and metrics](sparkplug-messages-and-metrics.md#node-metrics) |

## MQTT (MQTT)

| ID | MVP | Status | Evidence and limits | Detail |
|---|---|---|---|---|
| MQTT-1 | ✅ | done | Q: host, port, `client_id`, keepalive (a frozen broker found in 1 × keepalive). C: the user name and the password | [`mqtt`](configuration.md#mqtt) |
| MQTT-2 | ✅ | done | H + Q: a broker restart, a frozen broker, and a broker that cuts after the CONNACK | [`mqtt`](configuration.md#mqtt) |
| MQTT-3 | ✅ | done | Q: the NDEATH is the Will | [Sparkplug messages and metrics](sparkplug-messages-and-metrics.md#messages) |
| MQTT-4 | | done | H + Q: TLS 1.3, the CA from LittleFS, the name against an IP SAN, a wrong `server_name`, a missing CA. C: the public CA bundle | [Connect to a broker with TLS](../how-to/connect-to-a-broker-with-tls.md) |
| MQTT-5 | | done | H + Q: a TLS 1.2 broker that requires a client certificate. The key is in plain text in the flash | [Known limitations](known-limitations.md#tls-mqtt-4-mqtt-5) |
| MQTT-6 | | done | H + Q: failover to a backup broker, and the return to the primary broker | [The list of brokers](configuration.md#the-list-of-brokers) |
| MQTT-7 | ✅ | done | Static queues and buffers, `outbox.limit`, `SKIP_PUBLISH_IF_DISCONNECTED`, a budget of 8 s for each publish (#59). No stress test | [Known limitations](known-limitations.md#the-mqtt-session) |
| MQTT-8 | | done | H + Q: Clean Start, the Will, and the reason code 0x04 (#155) | [Sparkplug behavior](sparkplug-behavior.md#mqtt-5-mqtt-8-155) |

## Sparkplug B (SPB)

| ID | MVP | Status | Evidence and limits | Detail |
|---|---|---|---|---|
| SPB-1 | ✅ | done | H + P + Q | [Sparkplug messages and metrics](sparkplug-messages-and-metrics.md) |
| SPB-2 | ✅ | done | H + Q | [Sparkplug messages and metrics](sparkplug-messages-and-metrics.md#messages) |
| SPB-3 | ✅ | done | H + Q. In RAM only: 0 at each boot, +1 for each CONNECT packet (#161) | [Sparkplug behavior](sparkplug-behavior.md#bdseq-spb-3-161) |
| SPB-4 | ✅ | done | H + Q | [Node metrics](sparkplug-messages-and-metrics.md#node-metrics) |
| SPB-5 | ✅ | done | H: the wrap from 255 to 0. Q: the continuity | [Sparkplug messages and metrics](sparkplug-messages-and-metrics.md#messages) |
| SPB-6 | ✅ | done | H + Q | [Sparkplug behavior](sparkplug-behavior.md#the-devices-and-their-state-mb-9-61-214) |
| SPB-7 | ✅ | done | H + Q. NDATA and DDATA have no datatype (#159) | [Sparkplug messages and metrics](sparkplug-messages-and-metrics.md#messages) |
| SPB-8 | ✅ | done | H + Q. The aliases depend on the order of the tags | [Device metrics](sparkplug-messages-and-metrics.md#device-metrics) |
| SPB-9 | ✅ | done | H + Q. Also `POST /api/rebirth` and the shell `rebirth` (#171) | [NCMD](sparkplug-messages-and-metrics.md#ncmd) |
| SPB-10 | ✅ | done | H + Q: a metric that is not valid is discarded alone; a decode failure discards the command. The datatype and fraction checks (#163), the write limits (#196). H: a full bus queue discards the command | [DCMD](sparkplug-messages-and-metrics.md#dcmd) |
| SPB-11 | ✅ | done | H + Q: a simulated DO with the polarity of the board. C: the TCA9554 | [NCMD](sparkplug-messages-and-metrics.md#ncmd) |
| SPB-12 | ✅ | done | H + Q | [`devices[].tags`](configuration.md#devicestags) |
| SPB-13 | | done | H + Q: STATE `false` and `true`, an old STATE (#157), an offline STATE with backup brokers (#156) | [Sparkplug behavior](sparkplug-behavior.md#the-primary-host-application-spb-13) |
| SPB-14 | | done | H + Q: the RAM buffer, the replay, the spill to a LittleFS directory. C: SDMMC, FAT and the format. GPIO45 risk: off by default | [Store & forward](store-and-forward.md) |
| SPB-15 | | done | H + Q. The value is not kept after a restart | [Sparkplug behavior](sparkplug-behavior.md#node-controlscan-rate-spb-15) |
| SPB-16 | | done | H + P + Q. A host test (`test_worst_case_birth_fits`) encodes the worst DBIRTH in the payload buffer of 32 KiB (`GW_SPB_PAYLOAD_MAX`) | [Sparkplug behavior](sparkplug-behavior.md#quality-and-properties-spb-16) |
| SPB-17 | | done | H + P + Q. C: the RSSI (QEMU has no radio). A host test (`test_worst_case_nbirth_fits`) encodes the worst NBIRTH in the payload buffer. That NBIRTH has `Node Info/Modbus TCP` (#81), 64 template tags, and 8 parameters in each of 16 definitions (#276) | [Diagnostics](sparkplug-messages-and-metrics.md#diagnostics) |
| SPB-18 | | done | H + P + Q. The user defines the template parameters of each device type (#276). No test with a real host | [Sparkplug behavior](sparkplug-behavior.md#templates-spb-18) |
| SPB-19 | | done | H + P + Q. Bytes and File hold 64 B at most. File has no metadata | [Sparkplug behavior](sparkplug-behavior.md#dataset-bytes-and-file-spb-19) |

## Modbus RTU (MB)

| ID | MVP | Status | Evidence and limits | Detail |
|---|---|---|---|---|
| MB-1 | ✅ | done | Q at 9600 8N1. H: the end-of-frame wait at 1200 baud with parity. C: the other parities | [`rs485`](configuration.md#rs485) |
| MB-2 | ✅ | compile-only | QEMU has no RS485 mode. Nobody examined the RTS/DE timing. The UART ISR is in IRAM | [Known limitations](known-limitations.md#no-test-on-the-board-1-18) |
| MB-3 | ✅ | done | H: FC01 to FC04. Q: FC01, FC03, FC04 | [`devices[].tags`](configuration.md#devicestags) |
| MB-4 | ✅ | done | H: all four. Q: FC05, FC06, a 32-bit FC16. FC15 and FC16 for one register with `write_multiple` (#197). NaN and infinite values are refused | [DCMD](sparkplug-messages-and-metrics.md#dcmd) |
| MB-5 | ✅ | done | H + Q | [The tag types](configuration.md#the-tag-types) |
| MB-6 | | done | H + P + Q: exact 64-bit values, the four orders, bits and fields of bits (#202), texts. A text holds 32 ASCII characters at most | [Known limitations](known-limitations.md#modbus-mb-6-mb-15) |
| MB-7 | ✅ | done | H + Q | [`devices[].tags`](configuration.md#devicestags) |
| MB-8 | ✅ | done | H + Q | [`rs485`](configuration.md#rs485) |
| MB-9 | ✅ | done | H + Q: the probe with backoff of offline devices (#61). A healthy device keeps its period | [Sparkplug behavior](sparkplug-behavior.md#the-probe-of-an-offline-device-61) |
| MB-10 | ✅ | done | H + Q | [`devices`](configuration.md#devices) |
| MB-11 | | done | H + Q: 60 registers in one FC03, and `max_gap`. Default: block reads with gap 0 (`CONFIG_GW_MB_BLOCK_READS`, `CONFIG_GW_MB_BLOCK_MAX_GAP`) | [`devices`](configuration.md#devices) |
| MB-12 | | done | H + Q. `slow_poll_ms` is 10 × `poll_ms` by default. Only a full cycle puts a device online | [Fast and slow scan](configuration.md#fast-and-slow-scan) |
| MB-13 | | done | H + Q. The counters are in the shell, the log and `Node Info/Modbus` | [Diagnostics](sparkplug-messages-and-metrics.md#diagnostics) |
| MB-14 | | done | H + Q: a write goes out between two requests of a cycle. The wait is one transaction at most | [Architecture](architecture.md) |
| MB-15 | | done | H + Q. Off by default. No authentication. Never through the access point (#120). The local unit (#216, #234) | [`modbus_tcp`](configuration.md#modbus_tcp) |
| MB-16 | ✅ | done | Design: one task (`rs485_bus`) owns the bus. The writes and the Modbus TCP requests go through its queue | [Architecture](architecture.md) |

## Local I/O (IO)

| ID | MVP | Status | Evidence and limits | Detail |
|---|---|---|---|---|
| IO-1 | ✅ | done | H: the debounce and the polarity. Q: simulated pins. C: the real GPIOs | [Set up the local inputs and outputs](../how-to/set-up-the-local-io.md) |
| IO-2 | ✅ | done | H: the boot sequence and the bus errors of the expander. Q: simulated registers. C: the real I²C | [Local I/O](sparkplug-messages-and-metrics.md#local-io) |
| IO-3 | | done | H + Q: a simulated expander in RTC memory. C: the real I²C | [The safe state of the DOs](configuration.md#the-safe-state-of-the-dos) |
| IO-4 | | done | H + Q. Approximately 25 Hz at most with 20 ms of debounce. Fast counters with PCNT (#218): C | [The pulse counters](configuration.md#the-pulse-counters) |
| IO-5 | ✅ | done | Q: the states in the log. C: RMT and WS2812. Nobody examined the colors | [The status signals](status-signals.md) |
| IO-6 | | done | H + Q (in the log). C: LEDC and GPIO46 | [`local_io.buzzer`](configuration.md#local_iobuzzer) |
| IO-7 | | done | H + Q. C: the real I²C. Only `follow`, `invert` and `toggle` | [Local rules](configuration.md#local-rules) |

## Configuration (CFG)

| ID | MVP | Status | Evidence and limits | Detail |
|---|---|---|---|---|
| CFG-1 | ✅ | done | H: the largest document, JSON paths, duplicate keys. Q: a rejected file starts recovery mode | [The configuration document](configuration.md) |
| CFG-2 | ✅ | done | Q: LittleFS. The embedded fallback applies only when the file is missing | [Configuration](configuration.md) |
| CFG-3 | | done | H: the auth vectors, the lists of the page, the lockout (#64). Q: the page, the status, recovery mode, HTTPS. Each key of the configuration has a control in the Configuration tab (#221) | [Change the configuration](../how-to/change-the-configuration.md) |
| CFG-4 | | done | H + Q: validation with the path, dry run, atomic write, masked export, CSRF (#100), rebirth (#171) | [The REST API](rest-api.md) |
| CFG-5 | | done | H + Q: a simulated button. C: GPIO0, the LED and the buzzer | [Recover a gateway](../how-to/recover-the-gateway.md) |
| CFG-6 | | done | Q: `status`, `devices`, `config` (`show`, `load`, `check`), `restart`, `rebirth`, `mbread`, `ping`. No password. `CONFIG_GW_SHELL_WRITE_COMMANDS=n` makes it read-only. The web UI also has a Modbus test read (#182) | [`POST /api/modbus/read`](rest-api.md#post-apimodbusread) |
| CFG-7 | | done | H + Q: the secrets are in NVS, out of the configuration document. The NVS encryption with a key in eFuse is out of scope on the board (#133). Only the secure QEMU build has it (`sdkconfig.qemu-secure`, decision of #133, for #255) | [Commissioning and secrets](commissioning-and-secrets.md) |
| CFG-8 | | done | H + Q: the claim window. C: the open access point | [Set the first web password on site](../how-to/set-the-first-web-password.md) |

## Firmware (FW)

| ID | MVP | Status | Evidence and limits | Detail |
|---|---|---|---|---|
| FW-1 | | done | H + Q: download, upload, refusals, the trial, the confirmation, the rollback. C: the rollback for low memory | [Firmware updates](firmware-updates.md) |
| FW-2 | | out of scope | Secure Boot burns eFuses (#133). Only the secure QEMU build requires signed OTA images (`sdkconfig.qemu-secure`, decision of #133, for #255) | [Features that burn eFuses](../explanation/no-efuse-features.md) |
| FW-3 | | out of scope | Flash encryption burns eFuses (#133) | [Features that burn eFuses](../explanation/no-efuse-features.md) |
| FW-4 | ✅ | done | Q: `0.1.0+g<hash>[.dirty]` in `Node Info/Firmware`. A release build gives the version without the hash (#279) | [Node metrics](sparkplug-messages-and-metrics.md#node-metrics) |

## Reliability (REL)

| ID | MVP | Status | Evidence and limits | Detail |
|---|---|---|---|---|
| REL-1 | ✅ | done | H + Q: the core dump in flash, its download, summary and erase (#132). No test of the real TWDT: QEMU disables the watchdog | [Get and decode a core dump](../how-to/decode-a-core-dump.md) |
| REL-2 | ✅ | partial | The code of the project is static. lwIP and esp-mqtt allocate from the heap (bounded) | [Known limitations](known-limitations.md#memory-rel-2) |
| REL-3 | ✅ | done | Q | [Read the live log](../how-to/read-the-live-log.md) |
| REL-4 | | partial | H + Q: syslog. No logs over MQTT, by choice | [Known limitations](known-limitations.md#remote-syslog-rel-4) |
| REL-5 | | done | H + Q: `power-on`, then `software` after a Reboot | [Node metrics](sparkplug-messages-and-metrics.md#node-metrics) |
| REL-6 | | done | Q: the E2E check `REL-6 less than 10 s from the boot to the NBIRTH` in the main scenario compares the time from the start of QEMU to the NBIRTH with the limit of 10 s. No measurement on the board | [Tests](tests.md) |

## Quality (QA)

| ID | MVP | Status | Evidence and limits | Detail |
|---|---|---|---|---|
| QA-1 | ✅ | done | Unity suites in `idf/test/host/`, also with sanitizers and `-O2` | [Tests](tests.md) |
| QA-2 | ✅ | done | 0 warnings. The build reports the size | [Compiler warnings and the stack](compiler-warnings-and-stack.md) |
| QA-3 | | partial | QEMU only, no hardware. The harness retries only known emulator faults, one time (#86, #90, #92, #96) | [Tests](tests.md) |
| QA-4 | | done | `.github/workflows/ci.yml`, also the OTA scenarios with heap poisoning (#104) | [Tests](tests.md) |

## UMH Core (UMH)

| ID | MVP | Status | Evidence and limits | Detail |
|---|---|---|---|---|
| UMH-1 | | done | The how-to, the explanation and the reference (#168) | [Connect the gateway to UMH Core](../how-to/connect-to-umh-core.md) |
| UMH-2 | | done | Warnings with the JSON path (#169) | [Name checks](sparkplug-behavior.md#name-checks-169) |
| UMH-3 | | done | `sparkplug.naming: "umh"` (#173, #174). A host test encodes the worst NBIRTH of the preset | [The UMH preset](sparkplug-behavior.md#the-umh-preset-173-174) |
| UMH-4 | | done | `sparkplug.device_prefix` (#172) | [The device prefix](sparkplug-behavior.md#the-device-prefix-172) |
| UMH-5 | | done | The topic previews in the web UI (#170) | [UMH names and topics](umh-names-and-topics.md) |
| UMH-6 | | done | The optional E2E scenario `umh` with benthos-umh v0.16.2 (#175). Not in the CI | [Tests](tests.md) |
| UMH-7 | | done | `POST /api/rebirth` and the Rebirth button (#171) | [The REST API](rest-api.md) |
| UMH-8 | | not done | The reports to the UMH project are open (#177) | [Sparkplug names and the UNS](../explanation/sparkplug-names-and-the-uns.md) |
