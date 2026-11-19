# Product specification: RS485 (Modbus RTU) to MQTT Sparkplug B gateway

Version: 2.0 · Date: 2026-11-11

This document gives the requirements (§4), the minimum product or MVP (§5), the configuration schema (§6) and the acceptance criteria of the MVP (§7). [Architecture](reference/architecture.md) gives the design. [Requirements status](reference/requirements-status.md) gives the evidence for each requirement, and [Known limitations](reference/known-limitations.md) gives the known limitations.

## 1. Purpose

The gateway is an industrial edge device. It does these functions:

- It polls field devices on **RS485 with Modbus RTU**.
- It publishes their values to an **MQTT** broker with **Sparkplug B 3.0.0**.
- It writes the values of commands (NCMD and DCMD) from a SCADA or MES host.
- It publishes its 8 local digital inputs and its 8 local digital outputs as Sparkplug metrics.

In Sparkplug, the gateway is an *Edge Node*. Each Modbus slave is a Sparkplug *Device*.

**Open source.** The firmware runs on a board that anyone can buy. The owner of the board must always be able to read, flash and change it. Thus the firmware burns no eFuse and does not lock the board (FW-2, FW-3, CFG-7).

**Sparkplug conformance.** The gateway obeys each MUST, MUST NOT, SHOULD and SHOULD NOT of Sparkplug B 3.0.0 for an Edge Node. The reference is the normative text with a `tck-id`, at the tag `v3.0.0` of `eclipse-sparkplug/sparkplug`. When this document does not agree with that text, the Sparkplug text is correct, and this document must change. [Sparkplug behavior](reference/sparkplug-behavior.md#the-sparkplug-tck-edge-node-profile-176) gives the known FAILs of the Sparkplug TCK and their causes.

## 2. Hardware

### 2.1 Selection

| Candidate | Can you program it? | Result |
|---|---|---|
| Waveshare RS232/485/422 TO POE ETH (B) | **No.** It has closed firmware for a serial server. You can only configure it (web, PC tool or command set) | Not selected: it cannot run a Sparkplug B implementation of its own |
| **Waveshare ESP32-S3-POE-ETH-8DI-8DO** | **Yes.** It has an ESP32-S3, and ESP-IDF supports it | **Selected** |

### 2.2 One implementation

This repository has one implementation of this specification: the ESP-IDF v6.1 firmware in `idf/`.

During the selection, a Zephyr prototype was also made for a comparison. That prototype is not on `main`, and the project does not maintain it. The branches `impl/zephyr` and `comparison/idf-vs-zephyr` keep it. The project selected ESP-IDF for these reasons:

- The UART of the ESP32-S3 controls the RS485 direction in hardware. The Modbus subsystem of Zephyr prevents this.
- ESP-IDF runs tasks on the two cores. The Zephyr prototype ran on one core only.
- ESP-IDF has the Wi-Fi driver, OTA with rollback, and an MQTT client with keepalive and TLS.

The full comparison (in Spanish) is `docs/RECOMMENDATION.md` at the tag `archive/studies` ([commit d645ec6](https://github.com/tmijail/mqtt-rs485-gateway/blob/d645ec6df5c11da5d2e22fdeb5151bb00bfdd88d/docs/RECOMMENDATION.md)).

### 2.3 Resources of the ESP32-S3-POE-ETH-8DI-8DO

| Resource | Data | Pins |
|---|---|---|
| MCU | ESP32-S3-WROOM-1U-**N16R8**: 2 × Xtensa LX7 at 240 MHz, 16 MB of flash, 8 MB of octal PSRAM. The firmware requires the PSRAM (§2.5) | — |
| Ethernet | **W5500** on SPI, 10/100 | MOSI 13, MISO 14, SCLK 15, CS 16, INT 12, RST 39 |
| PoE | IEEE 802.3af module on the board | — |
| RS485 | Isolated, half-duplex, direction control with RTS | TX 17, RX 18, **RTS/DE 21** |
| CAN | Isolated, TWAI controller. **Not used** (§2.4) | TX 2, RX 3 |
| DI × 8 | Opto-isolated and bidirectional. They accept a dry contact or an active signal. An active input is a low level on the GPIO (pull-up) | GPIO 4–11 |
| DO × 8 | Opto-isolated Darlington outputs that sink up to 500 mA, with a flyback diode. An active output is a low level on the pin of the expander | EXIO1–8 of a TCA9554 I²C expander at address 0x20 |
| RTC | PCF85063 at address 0x51, on I²C, with an interrupt | SDA 42, SCL 41, INT 40 |
| RTC battery | A holder for a 1220 coin cell (12 mm diameter, 2 mm thickness). It accepts only a rechargeable 3 V cell, for example an ML1220. The box does not contain a cell. The gateway requires the cell (TIME-2, issue #160) | — |
| RGB LED | WS2812, RGB byte order | GPIO 38 |
| Buzzer | Passive (it needs PWM) | GPIO 46 |
| microSD | SDMMC, 1-bit mode | D0 45, CMD 47, CLK 48 |
| BOOT button | — | GPIO 0 |
| Wi-Fi / BLE | 2.4 GHz, in the ESP32-S3 | — |
| Power supply | 7–36 V on the terminal block, PoE, or 5 V on USB-C | — |

The TCA9554 of the outputs and the RTC use the same I²C bus (GPIO 41 and 42). Nobody found a published schematic. [Board data to verify](reference/board-data-to-verify.md) gives the source and the confidence of each item in this table.

> **Caution: Do not install a CR1220 cell in the RTC battery holder.** The board charges the cell. A primary (not rechargeable) cell can be damaged. Install a rechargeable 3 V cell, for example an ML1220. [Fit the RTC battery](how-to/fit-the-rtc-battery.md) gives the procedure.

> **Caution: Measure the polarity of the DOs with a multimeter before you connect loads.** The polarity of the DIs and of the DOs comes only from indirect evidence. The Kconfig options `CONFIG_GW_DI_ACTIVE_LOW` and `CONFIG_GW_DO_ACTIVE_LOW` change it.

### 2.4 The isolated CAN port: out of scope

The board has an isolated CAN port on the TWAI controller of the ESP32-S3 (TX GPIO 2, RX GPIO 3).

**Decision of the product owner (issue #184):** the firmware does not use the CAN port. The CAN port is out of scope until the product owner decides differently.

- No requirement of §4 uses the CAN port, and the configuration schema has no key for it.
- The firmware does not configure GPIO 2 and GPIO 3. GPIO 3 is also a strapping pin (the JTAG source).
- A requirement for the CAN port needs a new requirement ID and a decision of the product owner.

### 2.5 PSRAM

The firmware assumes the 8 MB of PSRAM of the board (issue #135). When the boot does not find the PSRAM, the gateway runs only the fault mode:

- It does not load the configuration. It does not poll, and it does not connect to MQTT. All DOs are off.
- The network, the web UI, the shell and the access point (NET-5) start. They show the cause.

[Architecture](reference/architecture.md#memory-rel-2) gives the details. Only the QEMU build without PSRAM (`CONFIG_GW_EMU_NO_PSRAM`) runs the gateway without PSRAM, for the tests.

## 3. Conventions

- Each requirement has an ID with the format `AREA-n`, for example `SPB-3`. An ID does not change. A requirement that is out of scope keeps its ID.
- The column **MVP** marks the requirements of the minimum product (§5).
- The column **Prio** gives the priority:
  - **P0**: necessary;
  - **P1**: expected in a good product;
  - **P2**: an advantage over other products;
  - **—**: out of scope.
- The column **Status** gives the state of the code on `main`:
  - **Done**: the firmware has the function, and host tests or the E2E in QEMU examine it;
  - **Partial**: the firmware has a part of the function. The cell tells which part is not done;
  - **Open**: the function is not done;
  - **Out of scope**: the project does not do it. The requirement gives the reason.
- **Done does not mean a test on hardware.** The firmware has never run on the board (issues #1 and #18). The W5500, the RS485 hardware mode, the TCA9554, the GPIO DIs, the LED and the radio are compile-only. [Requirements status](reference/requirements-status.md) gives the evidence for each requirement, and [Known limitations](reference/known-limitations.md) gives the known limitations.
- A text such as `#120` refers to a GitHub issue. A text such as `tck-id-…` refers to a normative statement of Sparkplug B 3.0.0.

## 4. Requirements

### 4.1 Network (NET)

| ID | Requirement | Prio | MVP | Status |
|---|---|---|---|---|
| NET-1 | Ethernet with the W5500 is the primary interface. The gateway detects the link state (up and down) and connects again automatically | P0 | ✅ | Done |
| NET-2 | IPv4 with DHCP or with a static address (IP, netmask, gateway and DNS). You can configure it | P0 | ✅ | Done |
| NET-3 | The gateway finds the address of the broker host with DNS | P0 | ✅ | Done |
| NET-4 | Wi-Fi STA as an alternative interface, or as a backup when Ethernet stops. You can configure the priority | P1 | | Done (compile-only: not tested with a radio) |
| NET-5 | A Wi-Fi access point for commissioning. It starts when there is no valid configuration, and when you push the BOOT button for 2 to 10 s. Its WPA2 key is the web password: the gateway keeps only the PMK of the password (issue #187). While no web password is set, the key is the commissioning password. During a claim window (CFG-8), it is open (no key). It stops on request (`POST /api/ap/stop`, the shell `ap stop`), after 5 min without a client in all modes (Kconfig `GW_SOFTAP_IDLE_MIN`), and in normal mode after 30 min at the latest (Kconfig `GW_SOFTAP_TIMEOUT_MIN`). The Modbus TCP gateway (MB-15) refuses each connection through the access point (issue #120) | P1 | | Done. Not tested with a radio |
| NET-6 | A hostname that you can configure, and an mDNS announcement of the web UI (`_https._tcp`, or `_http._tcp` without HTTPS) | P2 | | Done |
| NET-7 | IPv6 | P2 | | Done |

### 4.2 Time (TIME)

| ID | Requirement | Prio | MVP | Status |
|---|---|---|---|---|
| TIME-1 | SNTP synchronization with a server that you can configure. The Sparkplug timestamps are in ms UTC (`tck-id-payloads-timestamp-in-UTC`, `tck-id-payloads-metric-timestamp-in-UTC`). Until a source sets the clock (SNTP, the RTC or the browser with `POST /api/time`), the gateway opens no MQTT session: no CONNECT, no NBIRTH, no DATA and no store & forward sample. There is no time limit. The web UI, the REST API and the shell stay available. The web UI, the shell, the log and the LED show the wait and what to do (issue #160) | P0 | ✅ | Done |
| TIME-2 | The gateway reads the RTC at boot. It writes the RTC after each SNTP synchronization and after each time from the browser. Thus the timestamps are valid without a network. The RTC battery is necessary (§2.3). Without it, after each power cut the gateway sends no data until SNTP or an operator sets the time. When the RTC lost the time (OS bit at boot), the web UI, the shell and the log tell the operator to examine the battery | P1 | | Done |
| TIME-3 | The node metric `Node Info/Time Synced` tells if SNTP synchronized the clock | P2 | | Done |

### 4.3 MQTT (MQTT)

| ID | Requirement | Prio | MVP | Status |
|---|---|---|---|---|
| MQTT-1 | An MQTT 3.1.1 client on TCP. You can configure the host, the port, the client ID, the user name, the password and the keepalive. Each CONNECT has Clean Session = 1 (`tck-id-principles-persistence-clean-session-311`) | P0 | ✅ | Done |
| MQTT-2 | The gateway connects again automatically, with an exponential backoff and jitter (1 s to 60 s) | P0 | ✅ | Done |
| MQTT-3 | The MQTT Will (LWT) is the Sparkplug NDEATH (SPB-2) | P0 | ✅ | Done |
| MQTT-4 | TLS 1.2 or 1.3, with a CA that you can configure and a check of the host name | P1 | | Done |
| MQTT-5 | Mutual TLS with a client certificate and a client key | P1 | | Done |
| MQTT-6 | An ordered list of brokers, with failover | P1 | | Done |
| MQTT-7 | Bounded publish queues: a slow broker never causes a memory allocation without a limit | P0 | ✅ | Done |
| MQTT-8 | MQTT 5.0 | P2 | | Done |

### 4.4 Sparkplug B 3.0.0 (SPB)

**Topics:**

- `spBv1.0/{group_id}/{NBIRTH|NDEATH|NDATA|NCMD}/{edge_node_id}`
- `spBv1.0/{group_id}/{DBIRTH|DDEATH|DDATA|DCMD}/{edge_node_id}/{device_id}`
- `spBv1.0/STATE/{primary_host_id}`

The `device_id` is the device name, or `<device_prefix>:<name>` with `sparkplug.device_prefix` (§6.1, UMH-4).

| ID | Requirement | Prio | MVP | Status |
|---|---|---|---|---|
| SPB-1 | The payload is protobuf, as `sparkplug_b.proto` of Eclipse Tahu specifies | P0 | ✅ | Done |
| SPB-2 | The gateway registers the **NDEATH** as the MQTT Will: QoS 1, no retain (`tck-id-payloads-ndeath-will-message-qos`, `tck-id-payloads-ndeath-will-message-retain`). The NDEATH has the metric `bdSeq` with the datatype Int64 (`tck-id-message-flow-edge-node-birth-publish-will-message-payload-bdSeq`). It has no `seq` (`tck-id-payloads-ndeath-seq`). The `bdSeq` of the NBIRTH also has the datatype Int64 (`tck-id-message-flow-edge-node-birth-publish-nbirth-payload-bdSeq`). Before an intentional disconnection, the gateway publishes the NDEATH (`tck-id-operational-behavior-edge-node-intentional-disconnect-ndeath`). No message of an old session goes out after a new CONNECT (`tck-id-operational-behavior-primary-application-state-with-multiple-servers-walk`, #186) | P0 | ✅ | Done |
| SPB-3 | **bdSeq** from 0 to 255, in RAM only. It starts at 0 at each boot and increases by exactly 1 for each new MQTT CONNECT packet. After 255, it is 0 (`tck-id-topics-nbirth-bdseq-increment`, issue #161). A connection attempt that sends no CONNECT packet (DNS, TCP or TLS failure) does not use a value. The NBIRTH has the same value as the Will of that connection (`tck-id-topics-nbirth-bdseq-matching`) | P0 | ✅ | Done |
| SPB-4 | **NBIRTH**: QoS 0, no retain, `seq` = 0, with a timestamp (`tck-id-payloads-nbirth-qos`, `tck-id-payloads-nbirth-retain`, `tck-id-payloads-nbirth-seq`, `tck-id-payloads-nbirth-timestamp`). It contains `bdSeq`, `Node Control/Rebirth`, `Node Control/Reboot`, `Node Control/Scan Rate`, `Node Info/Firmware`, `Node Info/Hardware`, `Node Info/Reset Reason` and the local I/O metrics (§4.6). `Node Control/Rebirth` is a writable Boolean with the value false (`tck-id-payloads-nbirth-rebirth-req`). Each metric has a name, a datatype and a value (`tck-id-payloads-name-birth-data-requirement`, `tck-id-payloads-metric-datatype-req`). Each metric also has an alias, except `Node Control/Rebirth` (`tck-id-operational-behavior-data-commands-rebirth-name-aliases`, issue #153). If the publish of a birth fails, the gateway stops the session, and the next connection sends all the births again (issue #164) | P0 | ✅ | Done |
| SPB-5 | **seq** from 0 to 255, then 0 again. All the messages of the node use one sequence, except the NDEATH (`tck-id-payloads-sequence-num-incrementing`). A message that the gateway cannot encode does not use a number (issue #158) | P0 | ✅ | Done |
| SPB-6 | A **DBIRTH** for each Modbus device that is online, with all its metrics and their current values. The gateway sends it only after the NBIRTH (`tck-id-message-flow-device-birth-publish-nbirth-wait`). A **DDEATH** when the device goes offline (§4.5, MB-9, `tck-id-operational-behavior-device-ddeath`) | P0 | ✅ | Done |
| SPB-7 | **NDATA and DDATA** by exception: the gateway publishes only the metrics that changed. Each metric has its alias and a timestamp. It has no name and no datatype (`tck-id-payloads-metric-datatype-not-req`, issue #159): the host knows the datatype from the birth. A member of a template instance has its name and no datatype. The changes of one poll cycle go in one message | P0 | ✅ | Done |
| SPB-8 | **Aliases**: UInt64, unique in the node, declared in the births (`tck-id-payloads-alias-uniqueness`, `tck-id-payloads-alias-birth-requirement`) | P0 | ✅ | Done |
| SPB-9 | **Rebirth**: when the gateway receives the NCMD `Node Control/Rebirth` = true, it stops the NDATA and DDATA messages (`tck-id-operational-behavior-data-commands-rebirth-action-1`). Then it sends the NBIRTH and all the DBIRTHs again. `seq` starts again at 0, and the NBIRTH keeps the `bdSeq` of the session. `POST /api/rebirth` and the shell command `rebirth` do the same (issue #171) | P0 | ✅ | Done |
| SPB-10 | **DCMD**: the gateway writes writable metrics to Modbus. It finds the tag by alias or by name, and converts the type and the scale. After the write, it publishes the confirmed value. The gateway discards a metric that does not exist or is read-only. It also discards a metric with a datatype that is not the datatype of the DBIRTH (issue #163), and a value with a fraction for an integer or Boolean tag. It writes the other metrics of the command. A metric without a datatype uses the datatype of the DBIRTH. The gateway writes no metric of a command that does not decode, or whose writes do not fit in the queue of the bus. The log gives the reason | P0 | ✅ | Done |
| SPB-11 | **NCMD** for the local outputs `DO/n`, `Node Control/Reboot`, `Node Control/Scan Rate` (SPB-15), and `Node Control/Firmware Update`, `Node Control/Firmware SHA-256` and `Node Control/Boot Other Slot` (FW-1, only with `ota.mqtt`) | P0 | ✅ | Done |
| SPB-12 | An absolute deadband for each analog tag | P1 | ✅ | Done |
| SPB-13 | **Primary Host Application**: with a `primary_host_id`, the gateway subscribes to `spBv1.0/STATE/{id}`. It does not publish the NBIRTH until it receives `online: true`. When the host becomes offline, the gateway publishes the NDEATH and disconnects (`tck-id-operational-behavior-edge-node-termination-host-offline`) | P1 | | Done |
| SPB-14 | **Store & forward**: when the broker or the primary host is not available, the gateway keeps the changes in a ring buffer in PSRAM. An option moves them to a microSD card. When the session starts again, the gateway publishes them with `is_historical = true`. Without a valid clock (TIME-1), the gateway keeps nothing and sends no history | P1 | | Done |
| SPB-15 | A writable `Node Control/Scan Rate`, that changes the poll period of all devices | P2 | | Done |
| SPB-16 | Metric properties: `engUnit`, `engLow`, `engHigh` and the quality (`Quality`: 0 = BAD, 192 = GOOD) | P1 | | Done |
| SPB-17 | Diagnostic metrics of the node: `Node Info/Uptime`, `Free Heap`, the Modbus counters (OK, timeout, CRC, exception) and the Wi-Fi RSSI | P1 | | Done |
| SPB-18 | Sparkplug templates that describe the device types | P2 | | Done. No template parameters |
| SPB-19 | The `DataSet` payload, and the types `Bytes` and `File` | P2 | | Done |

### 4.5 Modbus RTU master on RS485 (MB)

| ID | Requirement | Prio | MVP | Status |
|---|---|---|---|---|
| MB-1 | A UART that you can configure: 1200 to 115200 baud, parity N, E or O, 1 or 2 stop bits, and 8 data bits | P0 | ✅ | Done |
| MB-2 | Half-duplex RS485 direction control with the RTS pin (GPIO 21). The gateway uses the hardware RS485 mode of the ESP32-S3 UART. A software GPIO is the alternative | P0 | ✅ | Done. Compile-only: QEMU has no RS485 mode |
| MB-3 | The read functions FC01 (coils), FC02 (discrete inputs), FC03 (holding registers) and FC04 (input registers) | P0 | ✅ | Done |
| MB-4 | The write functions FC05, FC06, FC15 and FC16. A 32-bit value always uses FC16 | P0 | ✅ | Done |
| MB-5 | The datatypes `bool`, `uint16`, `int16`, `uint32`, `int32` and `float32`, with a word order that you can configure (`big` = ABCD, `little` = CDAB) | P0 | ✅ | Done |
| MB-6 | The types `uint64`, `int64`, `float64` and `string`, the byte orders BADC and DCBA, one bit of a register (`bit_offset`: 0–15), and a field of 2 to 16 bits (`bit_width`, #202) | P1 | | Done |
| MB-7 | A linear scale `value = raw * scale + offset`, and the inverse for a write | P0 | ✅ | Done |
| MB-8 | A response timeout and retries that you can configure. The gateway keeps the silence of 3.5 characters between frames, and a delay between transactions that you can configure | P0 | ✅ | Done |
| MB-9 | Offline detection: after `offline_after` failed cycles in sequence, the device is offline (DDEATH). The first valid response makes it online again (DBIRTH) | P0 | ✅ | Done |
| MB-10 | A poll period for each device | P0 | ✅ | Done |
| MB-11 | **Block reads**: the gateway reads contiguous or near tags of the same area in one request. A block has up to 125 registers or 2000 bits, and a maximum gap that you can configure | P1 | | Done |
| MB-12 | A poll period for each tag, with a fast scan and a slow scan | P2 | | Done |
| MB-13 | The gateway decodes the Modbus exceptions (codes 1–11) and counts them for each device | P1 | | Done |
| MB-14 | The gateway queues the DCMD writes, and they have priority over the poll | P1 | | Done |
| MB-15 | A **Modbus TCP** gateway or server (port 502) that gives access to the RTU slaves through Ethernet. It always refuses a connection through the commissioning access point (NET-5, issue #120) | P2 | | Done |
| MB-16 | One owner of the RS485 bus: the poll and the writes go through one task or queue, one at a time | P0 | ✅ | Done |

### 4.6 Local I/O (IO)

| ID | Requirement | Prio | MVP | Status |
|---|---|---|---|---|
| IO-1 | 8 DIs as Boolean node metrics `DI/1`…`DI/8`, with a debounce that you can configure (20 ms by default). The gateway uses an interrupt, or it samples at 1 kHz or more | P0 | ✅ | Done |
| IO-2 | 8 DOs as writable Boolean node metrics `DO/1`…`DO/8`, through the I²C expander | P0 | ✅ | Done |
| IO-3 | **Safe state of the DOs** (failsafe), configurable for each output (hold, OFF or ON). It applies when MQTT is lost for more than N s, and at a restart | P1 | | Done |
| IO-4 | A pulse counter for each DI (UInt64), saved at intervals | P1 | | Done |
| IO-5 | An RGB status LED: no network, no broker, normal operation and a Modbus error, with documented colors or patterns | P1 | ✅ | Done. The colors have no test on the board |
| IO-6 | A buzzer for alarms that you can configure, and to confirm the factory reset | P2 | | Done |
| IO-7 | Local links from a DI to a DO (simple logic), also without a network | P2 | | Done |

### 4.7 Configuration (CFG)

| ID | Requirement | Prio | MVP | Status |
|---|---|---|---|---|
| CFG-1 | The configuration is a JSON document with the schema of §6. The gateway validates it when it loads it, and gives errors with the JSON path. With a configuration that is not valid, the gateway does not start the poll | P0 | ✅ | Done |
| CFG-2 | A default configuration in the firmware. A file in an internal file system (LittleFS) replaces it | P0 | ✅ | Done |
| CFG-3 | A **web UI** (HTTPS) with a user name and a password. You can edit the network, MQTT, Sparkplug, RS485 and the devices and tags, and see the live state | P1 | | Done |
| CFG-4 | A JSON REST API to read and write the configuration, with import and export | P1 | | Done |
| CFG-5 | A factory reset when you push the BOOT button for more than 10 s | P1 | | Done |
| CFG-6 | A serial console (shell) with commands for the state, the configuration and diagnostics | P1 | | Done |
| CFG-7 | The gateway keeps the secrets (passwords, TLS keys) out of the configuration document, in NVS. **Out of scope:** the encryption of the secrets with a key in eFuse on the board. The key in eFuse is permanent, and the owner of the board must keep full control of the board (#133). The secure QEMU build (`sdkconfig.qemu-secure`) encrypts NVS with emulated eFuses: #133 decided that this stays only there, for the optional owner profile of #255. The branch `archive/efuse-production` keeps the archived feature | P1 | | Done. The eFuse part is out of scope |
| CFG-8 | **First web access without the serial console** (issue #120): the claim window. While `web.password` is empty, a push of BOOT for 2 to 10 s opens a claim window for 10 min (Kconfig `GW_WEB_CLAIM_WINDOW_MIN`). During the window, a client of the access point (NET-5) can set the first web password without a credential (`POST /api/claim`, §6.1). Clients of other interfaces get `403`. The window closes at the end of its time, after the first password, when the access point stops, and at a restart. When a web password is set, the window does not open again. Only a factory reset (CFG-5) removes the password. Issue #187: 5 s after the claim, the access point changes from open to WPA2, with the new web password as its key | P1 | | Done |

### 4.8 Firmware management (FW)

| ID | Requirement | Prio | MVP | Status |
|---|---|---|---|---|
| FW-1 | **OTA** through HTTPS with A/B partitions, started from the web UI or by NCMD. A new image confirms itself with a local self-check; a reset or a failed self-check before that returns to the earlier image. A pre-flight check refuses an image that cannot read the stored data, a boot-loop counter starts a safe mode, and a button boots the other slot (issue #144) | P1 | | Done |
| FW-2 | **Out of scope:** Secure Boot. Secure Boot burns eFuses, and after it the board accepts only the firmware of the owner of the key. This is a vendor lock, and the project is open source (#133). The branch `archive/efuse-production` keeps the archived feature. The secure QEMU build (`sdkconfig.qemu-secure`) requires signed OTA images without Secure Boot: #133 decided that this stays only there, for the optional owner profile of #255 | — | | Out of scope |
| FW-3 | **Out of scope:** flash encryption. Flash encryption in release mode burns eFuses. After it, the owner cannot read or flash the board through USB (#133). The branch `archive/efuse-production` keeps the archived feature | — | | Out of scope |
| FW-4 | The semantic version and the git hash are in the NBIRTH (`Node Info/Firmware`) | P0 | ✅ | Done |

### 4.9 Reliability and observability (REL)

| ID | Requirement | Prio | MVP | Status |
|---|---|---|---|---|
| REL-1 | The task watchdog watches all critical tasks. After a panic, the core dump stays in flash, and the web API and the shell give it (issue #132) | P0 | ✅ | Done |
| REL-2 | Bounded memory: static limits for the devices and the tags, and no allocation in the poll, encode and publish path | P0 | ✅ | Partial. lwIP and esp-mqtt allocate from the heap |
| REL-3 | Logs with levels on the console UART | P0 | ✅ | Done |
| REL-4 | Remote syslog, or logs through MQTT | P2 | | Partial. Syslog only |
| REL-5 | The cause of the last restart, in the log and in the NBIRTH | P1 | | Done |
| REL-6 | Less than 10 s from the boot to the NBIRTH when the network and the broker are available | P1 | | Done in QEMU, no measurement on the board |

### 4.10 Quality (QA)

| ID | Requirement | Prio | MVP | Status |
|---|---|---|---|---|
| QA-1 | Unit tests on the host. They examine the Sparkplug encode and decode, the CRC and the Modbus frames, and the type and scale conversion. They also examine report by exception and the configuration parser | P0 | ✅ | Done |
| QA-2 | The ESP32-S3 build has no warnings in the code of the project, and it reports the use of flash and RAM | P0 | ✅ | Done |
| QA-3 | An end-to-end test on the host or in an emulator, with mosquitto and a simulated Modbus slave | P1 | | Partial. QEMU only, no hardware |
| QA-4 | CI that builds the firmware and runs the tests | P1 | | Done |

### 4.11 UMH Core (UMH)

UMH Core is a Unified Namespace (UNS) platform. Its bridge reads Sparkplug B with the `sparkplug_b` input of benthos-umh. These requirements make the gateway easy to use with UMH Core.

All these items obey Sparkplug B 3.0.0. None of them is a compatibility profile: the gateway does not change its default behaviour for UMH Core. When UMH Core does not obey Sparkplug, the project writes the limit in the documents and reports it to the UMH project (UMH-8).

| ID | Requirement | Prio | MVP | Status |
|---|---|---|---|---|
| UMH-1 | **Documents** (issue #168): a how-to (`docs/how-to/connect-to-umh-core.md`), an explanation (`docs/explanation/sparkplug-names-and-the-uns.md`) and a reference (`docs/reference/umh-names-and-topics.md`). They give the broker listener, the names, the gateway configuration, the UMH bridge configuration and the limits of UMH Core | P1 | | Done |
| UMH-2 | **Name checks** (issue #169): the validation finds the names that UMH Core and other UNS hosts change with no error. Sparkplug permits these names, thus they are warnings, each with its JSON path. The rules are in §6.1 | P2 | | Done |
| UMH-3 | **The UMH preset** (`sparkplug.naming: "umh"`, issues #169, #173 and #174). The name checks of UMH-2 become errors. Each device gets a Boolean node metric for its state (`Devices/<name>/Online`). A typed device publishes plain metrics, and the node publishes no DataSet. The preset is off by default (§6.1) | P2 | | Done |
| UMH-4 | **A device prefix of free depth** (issue #172): `sparkplug.device_prefix` gives the location levels of all devices. The `device_id` of each device is `<device_prefix>:<name>`. The number of levels has no limit, other than the length of the topic (§6.1) | P2 | | Done |
| UMH-5 | **The topic preview in the web UI** (issue #170): each device shows its Sparkplug topic and its UMH topic, with the length of the `device_id`. The previews change while you type. The fields have length limits and help texts | P2 | | Done |
| UMH-6 | **The interoperability test** (issue #175): the optional E2E scenario `umh` runs the real benthos-umh against the firmware in QEMU. It uses the bridge configuration of the how-to (UMH-1). [The E2E scenarios](reference/e2e-scenarios.md#umh-umh-core-interoperability-175) gives the checks | P1 | | Done. Optional scenario, not in the CI |
| UMH-7 | **Rebirth on request** (issue #171): `POST /api/rebirth` and the Rebirth button of the web UI send the NBIRTH and all the DBIRTHs again (SPB-9). Use them when a host shows the tags as `alias_<n>` after its restart | P2 | | Done |
| UMH-8 | **Reports to the UMH project** (issue #177): one upstream report for each gap of the `sparkplug_b` input. Examples: the Sparkplug 2.x STATE, Template, DataSet and Bytes values that become `null`, and no PropertySet | P2 | | Open |

## 5. MVP: summary

The MVP is a gateway that you start with a JSON file. It connects through Ethernet (DHCP or a static IP) to an MQTT broker without TLS. It publishes correct Sparkplug B 3.0.0, without a primary host and without store & forward. This includes:

- the birth and the death of the node and of the devices, the bdSeq (SPB-3), the seq and the aliases;
- report by exception with a deadband;
- rebirth, and writes by DCMD and NCMD.

The MVP polls N Modbus RTU slaves through the RS485 port of the board. It supports the 16-bit and 32-bit types, the scale, the timeouts, the retries and the offline detection. It also publishes the 8 DIs and the 8 DOs, and shows its state on the RGB LED.

**IDs of the MVP:**

- NET-1, NET-2, NET-3, TIME-1;
- MQTT-1, MQTT-2, MQTT-3, MQTT-7;
- SPB-1 to SPB-12;
- MB-1 to MB-5, MB-7 to MB-10, MB-16;
- IO-1, IO-2, IO-5;
- CFG-1, CFG-2, FW-4;
- REL-1 to REL-3, QA-1 and QA-2.

## 6. Configuration schema

This is the schema of the configuration document (CFG-1). The keys of this example are the keys of the MVP.

```json
{
  "network": { "dhcp": true, "ip": "192.168.1.50", "netmask": "255.255.255.0",
               "gateway": "192.168.1.1", "dns": "192.168.1.1" },
  "ntp":     { "servers": ["pool.ntp.org"] },
  "mqtt":    { "host": "192.168.1.10", "port": 1883, "client_id": "",
               "username": "", "password": "", "keepalive_s": 30 },
  "sparkplug": { "group_id": "Plant1", "edge_node_id": "GW01", "primary_host_id": "" },
  "rs485":   { "baud": 9600, "parity": "none", "stop_bits": 1,
               "response_timeout_ms": 200, "retries": 2, "inter_frame_delay_ms": 5 },
  "local_io": { "counter_save_s": 600 },
  "devices": [
    {
      "name": "Meter1", "unit_id": 1, "poll_ms": 1000, "offline_after": 3,
      "tags": [
        { "name": "Voltage", "area": "input",   "address": 0, "type": "float32",
          "word_order": "big", "scale": 1.0, "offset": 0.0, "deadband": 0.1,
          "writable": false, "units": "V" },
        { "name": "Setpoint", "area": "holding", "address": 10, "type": "int16",
          "scale": 0.1, "writable": true },
        { "name": "Run",      "area": "coil",    "address": 0, "type": "bool",
          "writable": true }
      ]
    }
  ]
}
```

**Ranges and defaults of the MVP keys.** The sections `mqtt` and `sparkplug` are necessary. The other sections are optional.

| Key | Default | Rule |
|---|---|---|
| `network.dhcp` | `true` | With `false`, `ip`, `netmask` and `gateway` are necessary |
| `ntp.servers` | `["pool.ntp.org"]` | 1 to 3 names, IPv4 or IPv6 addresses of SNTP servers, asked in order (#201) |
| `mqtt.host` | — | Necessary |
| `mqtt.port` | `1883` | 1–65535 |
| `mqtt.keepalive_s` | `30` | 5–3600 |
| `sparkplug.group_id`, `sparkplug.edge_node_id` | — | Necessary. 1–32 bytes, without `/ + #` |
| `rs485.baud` | `9600` | 1200–115200 |
| `rs485.parity` | `none` | `none`, `even` or `odd` |
| `rs485.stop_bits` | `1` | 1 or 2 |
| `rs485.response_timeout_ms` | `200` | 10–5000 |
| `rs485.retries` | `2` | 0–5 |
| `rs485.inter_frame_delay_ms` | `5` | 0–1000 |
| `local_io.di[].debounce_ms` | `20` | 0–1000, for each DI (#211) |
| `local_io.di[].invert` | `false` | Inverts the DI, after the polarity of the board (#211) |
| `devices[].name` | — | Necessary. 1–39 bytes |
| `devices[].unit_id` | — | Necessary. 1–247 |
| `devices[].poll_ms` | `1000` | 50–3 600 000 |
| `devices[].offline_after` | `3` | 1–1000 |
| `devices[].tags` | — | Necessary. 1–64 tags |
| `tags[].name` | — | Necessary. 1–39 bytes |
| `tags[].address` | — | Necessary. 0–65535 |
| `tags[].scale` | `1` | Not 0 |

**Rules of the schema:**

- `area` is one of `coil`, `discrete`, `holding` and `input`.
- `type` is one of `bool`, `uint16`, `int16`, `uint32`, `int32` and `float32`. Out of the MVP, `type` can also be `uint64`, `int64`, `float64`, `string` and `bytes` (§6.1, MB-6, SPB-19).
- `bool` is only for `coil` and `discrete`. Out of the MVP, `bool` can also be one bit of a `holding` or `input` register (key `bit_offset`, §6.1).
- `writable` is only for `coil` and `holding`.
- A `bool` tag has no `scale` and no `offset`.
- If `client_id` is empty, the gateway uses `edge_node_id`.
- Unknown keys and duplicate keys are errors.
- Names (issue #162):
  - `group_id`, `edge_node_id`, `primary_host_id`, the device `name` and `type`, and the tag `name` and `units` must be valid UTF-8.
  - The Sparkplug IDs must not contain the control characters U+0080–U+009F.
  - Two device names must not differ only in case (`tck-id-case-sensitivity-sparkplug-ids`). Two device types, or two tag names of one device, must not differ only in case (`tck-id-case-sensitivity-metric-names`). The comparison folds ASCII letters only.
  - A tag name must not also be a folder of another tag name of the same device (`A` and `A/B`).
  - A device `type` must not be the name or the first folder of a node metric (`bdSeq`, `DI`, `DO`, `Counter`, `Node Control`, `Node Info`, `Firmware Update`, `Run Hours`).
- With `sparkplug.device_prefix` (§6.1, #172), the `device_id` of a device is `<device_prefix>:<name>`. The longest device topic (`spBv1.0/<group_id>/DBIRTH/<edge_node_id>/<device_id>`) must fit in 159 bytes.

**Limits:** a maximum of 16 devices, 64 tags for each device and 256 tags in total (REL-2).

**Datatypes of the tags in Sparkplug:**

| Tag type | Sparkplug datatype |
|---|---|
| `bool` | Boolean (11) |
| `int16` | Int16 (2) |
| `uint16` | UInt16 (6) |
| `int32` | Int32 (3) |
| `uint32` | UInt32 (7) |
| `float32` | Float (9) |
| `int16`, `uint16` or `float32` with `scale` ≠ 1 or `offset` ≠ 0 | Float (9) |
| `uint64` / `int64` (MB-6) | UInt64 (8) / Int64 (4), exact in `long_value` |
| `float64` (MB-6). Also `int32`, `uint32`, `int64`, `uint64` or `float64` with `scale` ≠ 1 or `offset` ≠ 0. A Float has a 24-bit mantissa, thus a 32-bit counter above 2^24 loses its last digits in a Float (#165) | Double (10) |
| `string` (MB-6) | String (12) |
| `bool` with `bit_offset` (MB-6) | Boolean (11) |
| `bytes` (SPB-19) | Bytes (17) |

### 6.1 Optional keys out of the MVP

All these keys are optional. Without them, the gateway operates as the MVP. An unknown key is still an error.

**`network.wifi` (NET-4): Wi-Fi STA as an alternative or a backup for Ethernet.**

```json
"network": { "dhcp": true,
             "wifi": { "enabled": true, "ssid": "Plant", "password": "secret123",
                       "role": "backup", "failback_s": 30, "dhcp": true } }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `enabled` | bool | `false` | `false` = Ethernet only |
| `ssid` | text | `""` | 1–32 bytes. Necessary with `enabled=true` |
| `password` | text | `""` | Empty (open network), 8–63 characters (WPA2) or 64 hexadecimal digits (PSK) |
| `role` | `backup` \| `primary` | `backup` | `backup`: Ethernet gets the default route, and the gateway uses Wi-Fi only without Ethernet. `primary`: the opposite |
| `failback_s` | integer | `30` | 0–3600. The time that the preferred interface must be up without a cut before it gets the session again |
| `dhcp`, `ip`, `netmask`, `gateway`, `dns` | as in `network` | DHCP | The IPv4 data of the Wi-Fi interface. With `dhcp=false`, `ip`, `netmask` and `gateway` are necessary |

The IPv4 keys of `network` stay the keys of Ethernet.

**Path to the broker (issue #57).** An interface with a link and an address can have no path to the broker, for example when a router or a switch stops. If all brokers of the list (MQTT-6) fail on the active interface, the interface becomes "no service". The session then moves at once to the other interface, if it is usable, independently of its rank. The wait starts at 60 s, and each new failure in sequence makes it two times longer, up to 30 min (`CONFIG_GW_NET_NO_SERVICE_MIN_S` / `_MAX_S`). A stable session on that interface sets the wait to the minimum again. At the end of the wait, the preferred interface gets the session again, as in a normal failback: this is the retry. The gateway still uses an interface with no service when no other interface is usable.

**`network.hostname` and `network.mdns` (NET-6): the name of the gateway.**

```json
"network": { "dhcp": true, "hostname": "plant1-gw01", "mdns": true }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `hostname` | text | `""` | One DNS label: 1–63 letters, digits and `-` (not at the start or the end), no dots. Empty = a name from `edge_node_id`: lower case, and `-` for each character that is not a letter or a digit (`GW01` → `gw01`) |
| `mdns` | bool | `true` | The gateway answers `<hostname>.local` with mDNS. It announces the web UI as the service `_https._tcp` on `web.https_port` (`_http._tcp` on `web.port` with `web.https: false`). The instance name is `<group_id>/<edge_node_id>` |

The DHCP requests of Ethernet and Wi-Fi also contain the hostname.

**`network.ipv6` (NET-7): IPv6 together with IPv4.**

```json
"network": { "dhcp": true,
             "ipv6": { "enabled": true, "slaac": true, "address": "fd00:1::50", "dns": "" } }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `enabled` | bool | `false` | `false` = IPv4 only. With `true`, each interface has a link-local address |
| `slaac` | bool | `true` | Stateless address autoconfiguration from the router advertisements (RA) |
| `address` | text | `""` | A static address for Ethernet, in the text form of RFC 4291. The prefix on the link is /64. Multicast, link-local, `::` and `::1` are not permitted |
| `dns` | text | `""` | An IPv6 DNS server (the third position; DHCPv4 fills the first two positions) |

The gateway learns the default IPv6 router from the RAs. There is no static IPv6 gateway. `mqtt.host`, `ntp.servers` and `syslog.host` accept IPv6 literals (`fd00::10`) and names with an AAAA record.

**`mqtt.tls` (MQTT-4, MQTT-5): TLS to the broker, with an optional client certificate.**

```json
"mqtt": { "host": "broker.plant.local", "port": 8883,
          "tls": { "enabled": true, "ca_file": "ca.pem", "server_name": "",
                   "cert_file": "gw01.crt", "key_file": "gw01.key" } }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `enabled` | bool | `false` | `false` = TCP without encryption, as in the MVP. With `true`, the default of `mqtt.port` becomes `8883` |
| `ca_file` | text | `""` | A PEM file with the CAs that sign the certificate of the broker. It is in the LittleFS partition, together with `config.json` (`/cfg/<name>`). Empty = the bundle of public root CAs of ESP-IDF. A name of 1–32 characters `A-Z a-z 0-9 . _ -`, without `/`. It must not start with `.`, and it must not be `config.json` |
| `server_name` | text | `""` | The name that the certificate of the broker must contain (SAN or CN). Empty = the `host` of the connection (also an IP literal, against a SAN of the type IP). You cannot disable the name check |
| `cert_file` | text | `""` | MQTT-5: the PEM certificate of the client (mutual TLS), in LittleFS as `ca_file`. Empty = no client certificate |
| `key_file` | text | `""` | The PEM private key of the client, **not encrypted** (PKCS#1, SEC1 or PKCS#8). Necessary with `cert_file`, and not permitted without it |

The gateway uses TLS 1.2 or 1.3, as the broker selects. The gateway checks the validity dates of the certificate: the TLS connection waits for a valid clock (TIME-1). The gateway validates the TLS keys also when `enabled` is `false`.

**`mqtt.protocol` (MQTT-8): the protocol version.**

| Key | Type | Default | Rule |
|---|---|---|---|
| `protocol` | `"3.1.1"` \| `"5"` | `"3.1.1"` | The MQTT version for all brokers of the list. With `"5"`, the gateway obeys Sparkplug 3.0.0 on MQTT 5: *Clean Start* = 1 and *Session Expiry Interval* = 0 (`tck-id-principles-persistence-clean-session-50`). Thus the broker keeps no session between connections. *Will Delay Interval* = 0: the broker publishes the NDEATH when the connection stops. Each DISCONNECT of the gateway has the reason code 0x04, *Disconnect with Will Message* (`tck-id-payloads-ndeath-will-message-publisher-disconnect-mqtt50`, issue #155). Thus the broker also publishes the NDEATH of the Will. The topics, the QoS, the retain flags and the payloads do not change |

**`mqtt.backup_brokers`, `failover_after`, `primary_return_s` (MQTT-6): the list of brokers.**

```json
"mqtt": { "host": "broker1.plant.local", "port": 1883,
          "backup_brokers": [ { "host": "broker2.plant.local" },
                              { "host": "10.0.0.20", "port": 1884 } ],
          "failover_after": 3, "primary_return_s": 0 }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `backup_brokers` | array | `[]` | Up to 3 backup brokers, in order, after `mqtt.host` (the primary broker). Each one has `host` (necessary, not empty) and `port` (default: `mqtt.port`). All brokers use the same credentials, `client_id`, keepalive and `tls`. With TLS, the gateway checks the name against the `host` of each broker (or `tls.server_name`) |
| `failover_after` | integer | `3` | 1–20. The number of failed attempts in sequence on a broker before the gateway goes to the next broker. After the last broker, it goes to the first broker |
| `primary_return_s` | integer | `0` | 0–86400. `0` = the gateway keeps a backup broker that operates until it fails. With N > 0, after N s of a stable session on a backup broker, the gateway ends the session in order (NDEATH). Then it tries the primary broker **one time**. If that attempt fails, it goes to the first backup broker |

An attempt fails if it does not give a stable session. These are failed attempts:

- a TCP or TLS connection that fails;
- a CONNACK that refuses the connection;
- a SUBSCRIBE that the broker refuses or does not answer;
- a session that stops before one keepalive (60 s maximum) after the NBIRTH.

A stable session sets the count to zero. Each new broker starts with the minimum backoff. With two interfaces (NET-4), all brokers of the list can fail (`failover_after` times each) on the active interface. Then that interface has "no service", and the session moves to the other interface (refer to `network.wifi`).

**`sparkplug.primary_host_id` (SPB-13): the Primary Host Application.** This is a key of the MVP; empty = no primary host. With an ID, the gateway subscribes to `spBv1.0/STATE/{id}`, in the same SUBSCRIBE as NCMD and DCMD. Before the NBIRTH, it waits for a Sparkplug 3.0.0 STATE: a retained JSON `{"online": true|false, "timestamp": <ms>}`.

- `online: true`: the gateway publishes the NBIRTH and the DBIRTHs.
- `online: false` during a session: the gateway publishes an explicit NDEATH (with the bdSeq of the session) and disconnects. The next session waits again.
- The gateway ignores a STATE with a `timestamp` that is less than the last accepted `timestamp` (an old retained message). The host keeps one STATE timestamp for each MQTT server (`tck-id-operational-behavior-host-application-multi-server-timestamp`). Thus the gateway keeps the last `timestamp` for each broker of the list (issue #157). It never compares the STATE of one broker with the `timestamp` of another broker. It keeps these values between sessions, but not after a restart.
- The gateway ignores, with a warning, a payload that is not this JSON (for example, the `ONLINE` of Sparkplug 2.x).

With backup brokers (MQTT-6):

- A valid STATE `false` on a broker makes the gateway disconnect and connect to the next broker of the list at once (issue #156). This also applies when `failover_after` is more than 1. It applies during a session (`tck-id-operational-behavior-edge-node-termination-host-offline-reconnect`) and while the gateway waits for the host (`tck-id-operational-behavior-primary-application-state-with-multiple-servers-walk`). It is not a failed attempt.
- If the gateway receives no STATE `true` in 30 s, it disconnects. This counts as a failed attempt on that broker.

With one broker, the gateway stays connected and waits.

**`sparkplug.store_forward` (SPB-14): store & forward.**

| Key | Type | Default | Rule |
|---|---|---|---|
| `store_forward` | bool or object | `false` | `false`: without a Sparkplug session, the gateway keeps only the last value of each metric. The next session publishes the current value (as in the MVP). `true`: the gateway keeps each change that it would publish (with the same exception and *deadband* rules) in a ring buffer. After the NBIRTH and the DBIRTHs of the next session, it publishes them from the oldest to the newest as DDATA or NDATA. Each one has `is_historical = true` and the time of the sample |

"Without a session" includes a broker that is not available and a primary host (SPB-13) that is offline, from the first NBIRTH of the boot. Before the first NBIRTH, there is no published state to compare with.

The buffer has 40 000 samples in PSRAM (960 KB, Kconfig `GW_SF_SAMPLES`). The QEMU build without PSRAM has 512 samples in internal RAM. When the buffer is full, the gateway writes over the oldest samples. When the session starts again, a device can have no DBIRTH, because it is offline. The gateway discards the samples of that device. The buffer keeps 64-bit integers and texts (MB-6) exactly.

The object form, with the optional spill to a microSD card:

```json
"sparkplug": { "group_id": "Plant1", "edge_node_id": "GW01",
               "store_forward": { "enabled": true, "sd": { "enabled": false, "max_kb": 131072 } } }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `enabled` | bool | `true` | The same as `store_forward: true` |
| `sd.enabled` | bool | `false` | When the RAM buffer is three quarters full, its oldest samples go to the microSD card in blocks of 4 KB. When the session starts again, the gateway publishes the samples of the card first. It requires `enabled`. The QEMU build without PSRAM does not start the spill, and the boot log and the shell tell why. **GPIO45 (D0 of the card) is a *strapping* pin: read the warning in [Store & forward](reference/store-and-forward.md#the-microsd-card-and-gpio45) before you enable it** |
| `sd.max_kb` | integer | `131072` | 64–4 194 304 (128 MB by default). The space that the gateway uses on the card. It uses less when the cluster size limits it (refer to [Store & forward](reference/store-and-forward.md#extents-blocks-and-superblocks)). When the card is full of data, the gateway writes over the oldest blocks and counts their samples as lost |

**The card.**

- The card is only for the gateway. Do not put other files on it.
- Format the card with the gateway before the first use. Use the web UI (the status page, "microSD card", "Format the card") or the shell command `card format confirm`. The format makes one MBR partition with FAT32 and clusters of 32 KB (FAT16 below 2 GB).
- **The format deletes everything on the card**, also the samples on it. The gateway never formats a card by itself: the web UI asks you to type `FORMAT`, and the shell needs the word `confirm`.
- These cards do not mount: a card with no file system, an exFAT, NTFS or ext4 card, and a card with a GPT partition table. ESP-IDF v6.1 has no exFAT, and most cards of 64 GB or more have exFAT when you buy them. The gateway formats these cards too. The format does not need a mounted card, and it operates also when `sd.enabled` is `false`. The status page and `/api/status` (`card`) tell why a card does not mount.
- The QEMU build without PSRAM refuses the format and tells why.
- The format runs in the background. It can take some minutes on a large card. The spill stops first, and the RAM buffer continues. After the format, restart the gateway to use the card.
- A card that a PC formatted with clusters of 4 KB mounts, but the gateway then uses a maximum of 512 MB of it.
- Use an industrial card (pSLC) when you can. The gateway writes each block one time, with no FAT or directory write in normal operation.
- The gateway keeps the data in the folder `/sf`. The ring is in the files `r0000.sf`, `r0001.sf` and the next numbers. The files `ringa.sf` and `ringb.sf` are the two copies of its superblock. A PC does not need to read them.
- The data on the card stays after a restart and after a power cut. The ring keeps a hash of the configuration fields that identify a sample (issue #145). When this hash changes, the gateway starts a new ring. The gateway does not publish the old data, and it deletes no file for it.
- The gateway deletes the files of the earlier card format (`NNNNNNNN.sf`) at the boot, and does not publish their data (issue #151).
- **These changes start a new ring:**
  - a device or a tag that you add, remove, rename or move;
  - a change of the datatype of a tag (from `type`, `scale` and `offset`, for example a scale on an `int16` tag);
  - a change of the `type` (template) of a device, of `sparkplug.group_id`, of `sparkplug.edge_node_id`, of `sparkplug.device_prefix` or of `sparkplug.naming`;
  - a change of the `length` of a `bytes` tag.
- **These changes keep the data on the card:**
  - `poll_ms`, `slow_poll_ms`, `scan`, `unit_id`, `offline_after`, `block_reads`, `max_gap`, `area`, `address`, `bit_offset`, `bit_width`, `word_order`, `deadband`, `writable`, `units`, `eng_low`, `eng_high`;
  - a new `scale` or `offset` when the datatype stays the same;
  - the `length` of a `string` tag;
  - all other sections.

  A firmware update also keeps the data, because the hash does not use the memory layout of the firmware.

**Planned restart (issue #136).** Before a planned restart, the gateway moves the samples of the RAM buffer to the card. Then it writes the superblock. After the boot, it publishes these samples one time, in order, before the newer data.

- These restarts are planned: the shell `restart`, `POST /api/restart`, the NCMD `Node Control/Reboot`, the restart after a firmware update, and the restart after a saved configuration.
- The move has a time limit of 5 s. At the end of the limit, the gateway restarts. The samples that are not on the card are then lost. The log tells how many samples moved and how many are lost.
- The gateway does not move the RAM buffer in these cases:
  - after a factory reset;
  - during a format of the card;
  - when the spill is off or failed;
  - before the boot read the card.
- A saved configuration can change the configuration hash (the list above). Then the next boot starts a new ring, and it does not publish the samples of the earlier ring. Thus the gateway does not move the RAM buffer before this restart (issue #141), and the log tells why.
- **An unplanned restart (a power cut, a watchdog, a crash) loses the RAM buffer.** Only the data that was already on the card stays.

**Retention.** The gateway keeps only the changes, not each poll: a tag goes into the buffer only when it changed by more than its `deadband`. Thus calculate the retention in changes per second. One change is one sample of 24 B (a text: 1 + length / 8 samples), and one block of 4 KB holds 169 samples. Thus 1 MB of `max_kb` holds 43 264 samples:

- hours ≈ `max_kb` × 0.01174 / (changes per second);
- the default 128 MB at 50 changes per second: approximately 31 h; at 5 changes per second: approximately 13 days.

The RAM buffer adds 40 000 samples. The `deadband` is the main control: set it a little above the noise of the sensor.

**Duplicates after a restart (for the host or historian).** The gateway saves its replay position on the card after each 64 KB of published history (2 730 samples). It saves the position after the publish of those samples returned.

- Thus a restart or a power cut during a replay can send a maximum of 64 KB of historical samples again (approximately 2 700).
- A planned restart saves the position first.
- If the power cut damages the save itself, the maximum is 128 KB. If both superblock copies are damaged, the gateway can send all the data on the card again.
- The duplicates have the same timestamps and values as the first copy. The host or historian must remove them: for example, it keeps one value for each tag and timestamp. A counter or an aggregate of the received messages counts them two times.
- The history uses QoS 0: a power cut can also lose the messages that the TCP connection did not send yet.

**64-bit types, `string`, byte orders and register bits (MB-6), in each tag.**

```json
{ "name": "Energy",  "area": "holding", "address": 30, "type": "uint64" },
{ "name": "Counter", "area": "holding", "address": 34, "type": "int64", "word_order": "CDAB", "writable": true },
{ "name": "Flow",    "area": "input",   "address": 40, "type": "float64", "word_order": "DCBA" },
{ "name": "Serial",  "area": "holding", "address": 50, "type": "string", "length": 8 },
{ "name": "Alarm",   "area": "holding", "address": 60, "type": "bool", "bit_offset": 3, "writable": true }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `type` | also `uint64` \| `int64` \| `float64` \| `string` | — | 64 bits = 4 registers. `string` = `length` registers |
| `word_order` | `big` \| `little` \| `ABCD` \| `CDAB` \| `BADC` \| `DCBA` | `big` | The letters are the bytes of the value, from the most significant (A) to the least significant, in the order of arrival. `big` = `ABCD`. `little` = `CDAB` (the least significant word first). `BADC` = the bytes of each register in the opposite order. `DCBA` = all bytes in the opposite order. This applies to 32 and 64 bits. For 16 bits, only the byte swap applies |
| `length` | integer | — | 1–16 registers. Necessary with `type: string`, and only with it. Each register holds two ASCII characters. The first character is in the high byte (in the low byte with `BADC` or `DCBA`). The word order does not apply |
| `bit_offset` | integer | — | 0–15, with `type: bool` in `holding` or `input` (necessary there), or with `type: uint16` and `bit_width` 2–16 (#202). 0 = the least significant bit of the register, as it arrives. With `bit_offset`, `word_order` must be `big` |
| `bit_width` | integer | `1` | 1–16, only with `bit_offset`. 1 = `bool`; 2–16 = a field of bits, `uint16`. `bit_offset` + `bit_width` ≤ 16 |

- **Exact values.** The gateway publishes `int64` and `uint64` without a scale exactly (UInt64 or Int64), also above 2^53. With `scale` or `offset`, they become a Double.
- **`string`:** the gateway cuts the text at the first NUL and removes the spaces at the end. It publishes each character that is not printable ASCII as `?`. For a write, it adds NUL characters at the end. It refuses a text that is longer than `2 × length`. A `string` tag has no `scale`, no `offset` and no `deadband`.
- **Write of a bit:** the gateway reads the register (FC03). Then it writes the full register with the changed bit (FC06, or FC16 when multiple writes are forced).

**Raw register block (SPB-19), `type: bytes`.**

```json
{ "name": "Block", "area": "holding", "address": 220, "type": "bytes", "length": 3, "writable": true }
```

- The gateway publishes `length` registers (1–16, necessary) as a **Bytes** metric (17). Each register gives two bytes, the high byte first (the low byte first with `BADC` or `DCBA`; the word order does not apply). The gateway does not change the bytes: a NUL or a byte that is not ASCII stays as it is.
- A `bytes` tag has no `scale`, `offset`, `deadband`, `eng_low` or `eng_high`. Report by exception compares all the bytes.
- A DCMD must have a Bytes value of exactly `2 × length` bytes. The gateway writes it with one FC16.

**Metric properties (SPB-16), in each tag.**

```json
{ "name": "Voltage", "area": "input", "address": 0, "type": "float32", "units": "V",
  "eng_low": 0, "eng_high": 400 }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `units` | text | `""` | Up to 15 characters. The DBIRTH gives it as the property `engUnit` (String). Empty = no property |
| `eng_low` | number | — | The property `engLow` (Float) in the DBIRTH. In the range of a Float (±3.4e38). Not permitted on `bool`, `string` and `bytes` tags |
| `eng_high` | number | — | The property `engHigh` (Float) in the DBIRTH. With the two keys, `eng_high` > `eng_low` |

- **Quality (`Quality`, Int32):** the value is 192 (GOOD) when the read got a response. It is 0 (BAD) when the read failed (a Modbus exception). It is also 0 when the gateway never read the tag. With BAD, the value is also `is_null`. Each DBIRTH has the quality in each metric. A DDATA has it only when it changes. The node DOs use the same rule: BAD while the expander does not respond.
- The properties are in the `PropertySet` of the metric (keys and values in the same order). The gateway accepts a command (NCMD or DCMD) with properties, and ignores its properties.

**Block reads for each device (MB-11), in each device.**

```json
{ "name": "Meter1", "unit_id": 1, "max_gap": 10, "tags": [ … ] },
{ "name": "Picky",  "unit_id": 7, "block_reads": false, "tags": [ … ] }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `block_reads` | bool | the firmware value (`true`) | `false` = one request for each tag. Use it for a slave that answers with an exception if a block contains registers that it does not map |
| `max_gap` | integer | the firmware value (`0`) | 0–1999. The number of unused registers or bits that a block can include to join two tags of the same area. Not permitted with `block_reads: false`. A block never has more than 125 registers or 2000 bits |

Without these keys, the build option applies (`CONFIG_GW_MB_BLOCK_READS`, `CONFIG_GW_MB_BLOCK_MAX_GAP`), as in the MVP.

**Device types as Sparkplug templates (SPB-18), in the section `device_types` (#213).** The tag list of a type is in the document one time. A device with `type` has no `tags`.

```json
"device_types": [ { "name": "Pump", "tags": [ … ] } ],
"devices": [ { "name": "PumpA", "unit_id": 1, "type": "Pump" },
             { "name": "PumpB", "unit_id": 2, "type": "Pump" } ]
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `device_types[].name` | text | — | A maximum of 39 characters, valid UTF-8, no `/ + #`. Not the name or the folder of a node metric (§6, issue #162). Two types must not differ only in case. A maximum of 16 types |
| `device_types[].tags` | array | — | 1–64 tags, with the keys of a tag |
| `devices[].type` | text | — | The name of an entry of `device_types`; another name is an error. Without it, the device has its own `tags` (no template, one metric for each tag, as in the MVP) |

- Each device of a type gets a copy of the tags of the type. The copies count in the limit of 256 tags.
- The definitions of the types that devices use describe a maximum of **64 tags** together (each type counts one time), because the NBIRTH contains all of them.
- **NBIRTH:** one Template metric (19) for each type that a device uses, with its name, `is_definition = true`, no alias and no `template_ref` (`tck-id-payloads-template-definition-nbirth`). Its members have a name, a datatype and a default value (0, false or an empty text). They also have the properties `engUnit`, `engLow` and `engHigh` of the tags of the type.
- **DBIRTH:** one Template metric, the instance. Its name is the type, its alias is 100 + the first tag of the device, and its `template_ref` is the type. Its members are the tags, by name and without an alias, with a value and `Quality`.
- **DDATA:** the instance by alias, with only the members that changed (by name).
- **DCMD:** the instance with the members to write (by name). The gateway also accepts the tag by name as a separate metric. It does not accept a member by alias, because the members have no alias.
- The gateway does not use template parameters. With the UMH preset (UMH-3), a typed device publishes plain metrics.

**Fast and slow scan (MB-12).**

```json
{ "name": "Meter1", "unit_id": 1, "poll_ms": 1000, "slow_poll_ms": 30000, "tags": [
    { "name": "Voltage", "area": "input",   "address": 0,  "type": "float32" },
    { "name": "Serial",  "area": "holding", "address": 50, "type": "string", "length": 8, "scan": "slow" } ] }
```

| Key | Where | Type | Default | Rule |
|---|---|---|---|---|
| `scan` | tag | `fast` \| `slow` | `fast` | The gateway reads a `fast` tag each `poll_ms`, and a `slow` tag each `slow_poll_ms` |
| `slow_poll_ms` | device | integer | 10 × `poll_ms` (maximum 3 600 000) | 50–3 600 000, and not less than `poll_ms` |

- Fast tags and slow tags are never in the same read block.
- **Offline (MB-9):** a cycle that reads only the fast tags counts as any other cycle: it fails if a request gets no response. But a device becomes online (or online again) only after a cycle that read **all** its blocks. Thus its DBIRTH contains all the tags. While the device is offline, each cycle includes the slow tags.
- If a cycle fails before it reads the slow tags, the next cycle reads them.
- The gateway polls a device with only slow tags each `slow_poll_ms`.

**`modbus_tcp` (MB-15): a Modbus TCP gateway to the RTU slaves.**

```json
"modbus_tcp": { "enabled": true, "port": 502, "allowed_clients": ["192.168.1.20", "10.0.5.0/24"],
                "idle_timeout_s": 60 }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `enabled` | bool | `false` | Modbus TCP **has no authentication**: the server starts only when you enable it |
| `port` | integer | `502` | 1–65535 |
| `allowed_clients` | array of texts | `[]` | Up to 8 IPv4 addresses (`a.b.c.d`) or networks (`a.b.c.d/n`, n 0–32). The gateway closes a connection from another source without a response. Empty = any client (the log gives a warning). A connection through the commissioning access point is always refused, also when this list contains the client (see below) |
| `idle_timeout_s` | integer | `60` | 0–3600. The gateway closes a connection without requests for this time. 0 = never |

- The gateway sends FC01–FC06, FC15 and FC16 to the units 1–247, with or without a configured device. The gateway itself answers with an exception: any other function (01), data that is not valid (03), and the units 0 and 248–255 (0A). If the unit does not respond after the retries of `rs485`, the exception is 0B.
- **The access point (issue #120):** the gateway always refuses a connection through the commissioning access point (NET-5). This applies in all modes, with or without a claim window (CFG-8), also when `allowed_clients` contains the client. While the access point runs, the gateway refuses a connection when one of these conditions is true:
  - The local address of the connection is the address of the access point.
  - The client is in the subnet of the access point (`192.168.4.0/24`). Thus the gateway also refuses a plant client in this subnet while the access point runs.

  The gateway closes the connection without a response. It writes a log line and counts a rejected connection, as for a client that is not in `allowed_clients`.
- **Bus access (MB-16):** the TCP requests go through the task that owns the bus, one at a time. They have the same granularity as the DCMD writes: between two poll requests. The priority is: DCMD writes, then Modbus TCP, then the poll.

**`ota` (FW-1): firmware updates by NCMD and through the web UI.**

```json
"ota": { "mqtt": false, "ca_file": "fw-ca.pem" }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `mqtt` | bool | `false` | Issue #223: `true` accepts the NCMD `Node Control/Firmware Update` (and `Node Control/Firmware SHA-256`, issue #215). It also accepts `Node Control/Boot Other Slot`. With `false` these metrics are not in the NBIRTH, and the gateway refuses them. An update from the web UI is always possible |
| `ca_file` | text | `""` | The CA (PEM, in LittleFS next to `config.json`, the same name rules as `mqtt.tls.ca_file`) of the server of the images. Empty: the one of `mqtt.tls.ca_file` if TLS is active; else the public root CAs of ESP-IDF |

- **Start:** an NCMD with the metric `Node Control/Firmware Update` (String, the `https://…` URL; only with `ota.mqtt`) or `POST /api/firmware` of the web (`{"url": "…", "sha256": "…"}`, or the image as the body with the optional header `X-Image-SHA256`). With a SHA-256 (issue #215), the gateway does not install a file whose SHA-256 does not agree. The URL has up to 255 characters; the gateway rejects a longer one with its cause (issue #87). Only `https://`; the image must be of this project. The QEMU build also requires the signature of its test key, until #133 decides (FW-2).
- **Result:** the node metrics `Firmware Update/State` (String) and `Firmware Update/Progress` (UInt64, %), in the NBIRTH and by NDATA when they change. The state is `idle`, `downloading <v>`, `restarting into <v>`, `testing <v>`, `installed <v>`, `failed: <cause>` or `rolled back from <v>: <cause>`. Every NBIRTH publishes the result of the last update, also after a restart (issue #144).
- **Trial and rollback (issue #144):** the new image boots on trial. A self-check of the gateway confirms it at the first instant when the uptime is at least 60 s, the gateway is in its expected mode, the network driver started and the loop of the web server is alive. The self-check does not depend on the broker, the Modbus slaves, NTP or the update server. If the self-check is not true at 5 min, the gateway rejects the image. A rejection, and any reset during the trial (crash, watchdog, power cut), return to the earlier image.
- **Expected mode:** an image installed from normal mode must reach normal mode; if it rejects the configuration, the gateway returns to the earlier image at once, with the JSON path in the state. An image installed from recovery mode, safe mode, commissioning, or with force, must reach recovery mode or better; then it stays, and the operator corrects the configuration in the web UI.
- **Pre-flight check:** before any write, the gateway refuses an image that cannot read the stored configuration (each key and enum value has a "since" schema number; the image declares the highest schema that it reads), the NVS or LittleFS. The refusal lists the JSON paths that are too new. An image that declares no schema (older than issue #144) is unknown. The web UI and the API can force such an image; the NCMD cannot.
- **The configuration from before the update (issue #146):** the first configuration change after an update keeps the earlier document (`config.prev.json`). A downgrade to the version of that document restores it before the restart, when the image cannot read the stored configuration and the configuration did not change since. Otherwise the refusal also lists the JSON paths that changed. In recovery mode, the web UI can restore the earlier document.
- **During the trial**, the gateway refuses configuration saves, updates and the other-slot button (`409`, `testing <v>: N s left`). A factory reset stays allowed.
- **Safe mode:** three crash resets in a row (panic, watchdog, CPU lock-up) start safe mode: recovery mode without the configuration, with the network, the web UI, the shell and the firmware upload. Safe mode ends after 60 min (one more try of normal mode), at a restart by the operator or at a power cycle.
- **The other slot:** the web UI shows both slots. The button "Restart into `<version>`" (`POST /api/firmware/other`) and the NCMD `Node Control/Boot Other Slot` (Boolean; only with `ota.mqtt`) boot the image of the other slot on trial, after the pre-flight check. A manual rollback uses this button, or an upload of the earlier image.

**`local_io`: local I/O without a network. Safe state of the DOs (IO-3).** All keys of `local_io` are optional (IO-3, IO-4, IO-6 and IO-7).

```json
"local_io": { "failsafe_s": 30,
              "do_failsafe": ["off", "on", "hold", "off", "off", "off", "off", "off"] }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `failsafe_s` | integer | `0` | 0–86400. After this number of seconds without a Sparkplug session (without a valid NBIRTH), each DO goes to its safe state. 0 = a loss of MQTT does not change the DOs |
| `do_failsafe` | array of texts | all `off` | Up to 8 items, `DO/1` first. Each item is `off`, `on` or `hold`. An item that is not there is `off` |

- **At a restart** (power-on, software restart, panic or watchdog): `off` and `on` go to that state when the configuration is valid. `hold` keeps the state that the TCA9554 expander kept during the restart. After a power cut, the expander is also off: `hold` starts as OFF.
- **Before the gateway loads the configuration**, all DOs go to OFF. The exceptions are the `hold` outputs of the last valid configuration (saved in NVS): the gateway does not change them. The `on` outputs become ON only after the validation of the configuration.
- **Rejected configuration** (recovery mode) or commissioning: all DOs go to OFF, also the `hold` outputs. If the expander did not respond at the boot, the gateway tries again with the same backoff. It sets all DOs to OFF when the expander responds.
- **No session during `failsafe_s`** (counted from the boot or from the loss): the `off` and `on` outputs go to that state one time. The `hold` outputs do not change. When the session starts again, the DOs stay as they are until the next NCMD. The NBIRTH gives their real state.
- Without these keys, the gateway operates as the MVP: all DOs are OFF at a restart, and a loss of MQTT does not change them.

**Pulse counters of the DIs (IO-4), in `local_io`.**

```json
"local_io": { "counter_save_s": 600 }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `counter_save_s` | integer | `600` | 0 or 60–86400. The interval for the save of the counters in NVS, only if they changed. 0 = no save (they are 0 again at each boot) |

- Each DI counts its **activations**: a change from inactive to active, after the debounce. With a debounce of 20 ms, the maximum is approximately 25 Hz. A pulse that is shorter than the debounce does not count.
- The node metrics `Counter/1`…`Counter/8` (UInt64, alias 30–37) are in the NBIRTH. They are also in the NDATA of the DI change, by exception.
- At the boot, the gateway restores the counters from the last save. An orderly restart (NCMD Reboot, a new configuration, the `restart` of the shell or of the web UI) saves them first. A power cut, a panic or the watchdog loses the count since the last save (a maximum of `counter_save_s`).
- An NCMD cannot write them. The factory reset sets them to 0.

**Local links from a DI to a DO (IO-7), in `local_io.rules`.**

```json
"local_io": { "rules": [ { "di": 2, "do": 5, "mode": "follow" },
                         { "di": 3, "do": 6, "mode": "toggle" } ] }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `rules` | array of objects | `[]` | Up to 8 rules. Each DO is in one rule only |
| `di` | integer | — | 1–8, necessary |
| `do` | integer | — | 1–8, necessary |
| `mode` | `follow` \| `invert` \| `toggle` | `follow` | `follow`: DO = DI. `invert`: DO = not DI. `toggle`: each activation of the DI (the change to active, after the debounce) inverts the DO |

- The gateway calculates the rules itself, with or without a network or a broker. A DI change gets to the DO in a few milliseconds plus the debounce.
- **Priorities:** a DO with a rule belongs to the rule.
  - The gateway refuses an NCMD for it: the log tells this, and the NDATA confirms the real state.
  - The safe state after a loss of MQTT (IO-3) does not change it.
  - At a restart, it starts as any DO (`off` or `hold` in `do_failsafe`; `on` is not permitted for a DO with a rule). Then the gateway calculates the rule with the DIs of the boot. `follow` and `invert` take the value of their DI immediately. `toggle` starts from that boot state.
- The DOs without a rule operate as usual: NCMD with a session, safe state without it.

**Buzzer (IO-6), in `local_io.buzzer`.**

```json
"local_io": { "buzzer": { "alarms": ["config_error", "mqtt_down", "device_offline"],
                          "alarm_after_s": 30 } }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `alarms` | array of texts | `[]` | The alarms that sound: `config_error` (rejected configuration), `mqtt_down` (no Sparkplug session), `device_offline` (one or more Modbus devices offline). No item two times. Empty = the buzzer only confirms the BOOT button actions |
| `alarm_after_s` | integer | `30` | 0–3600. The time of `mqtt_down` or `device_offline` before the alarm sounds. `config_error` sounds immediately |

- **Patterns:** one beep is 150 ms of tone and 150 ms of silence. Each alarm sounds its burst each 10 s while its condition continues: `device_offline` 1 beep, `mqtt_down` 2 beeps, `config_error` 3 beeps. If there are a number of alarms, the alarm with the most beeps sounds.
- **`config_error`:** a rejected configuration cannot enable an alarm. Thus the alarms of the last valid configuration apply (saved in NVS). After a factory reset, it does not sound.
- **BOOT button.** These beeps sound with or without alarms:
  - 1 beep when you release the button after 2 to 10 s (access point, NET-5; claim window, CFG-8). During the boot, the gateway ignores the push and gives no beep: push the button again when the boot is complete (issue #187);
  - 1 beep when the claim window sets the web password;
  - 2 beeps when the factory reset starts (CFG-5).
- You cannot stop an alarm while its condition continues.
- **`mqtt_down` and the clock:** while the gateway waits for a valid clock (TIME-1, issue #160), there is no Sparkplug session. Thus `mqtt_down` also sounds then, if it is enabled. The operator must set the time or make SNTP available.

**Status LED (IO-5).** The patterns, from the highest to the lowest priority:

| State | Pattern |
|---|---|
| Factory reset in progress | White, continuous |
| BOOT pushed for more than 2 s | White, blinks at 4 Hz |
| Claim window open (CFG-8), or access point open (no key; NET-5, issue #187) | Blue and white, alternately (each color 250 ms) |
| Commissioning (no configuration after a reset) | Blue, blinks at 1 Hz |
| Safe mode (on request or after 3 crashes, issue #209) | Blue and amber, alternately (1 Hz) |
| Configuration not valid (recovery mode, but not safe mode) | Red, blinks at 4 Hz |
| No network (no IP) | Red, continuous |
| No valid clock: the MQTT session waits for SNTP, the RTC or the browser (TIME-1, issue #160) | Magenta, blinks at 1 Hz |
| No broker | Amber, blinks at 1 Hz |
| Modbus device offline | Green and orange, alternately (1 Hz) |
| Normal operation | Green, continuous |

While the access point runs with WPA2 (NET-5, issue #187), a short cyan flash (125 ms) each 2 s replaces the pattern of the table, from "Commissioning" to "Normal operation". Between the flashes, the LED shows that pattern.

**Device prefix (UMH-4, #172), in `sparkplug`.**

```json
"sparkplug": { "group_id": "acme", "edge_node_id": "acme:berlin:packaging:gw01",
               "device_prefix": "acme:berlin:packaging:line3" }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `device_prefix` | text | `""` | 0–96 bytes. Location levels with `:` between them. No empty level: no `:` at the start or the end, and no `::`. The characters of a Sparkplug ID (UTF-8, no `/ + #`, no control characters). The number of levels has no limit, other than the length |

- The `device_id` of each device is `<device_prefix>:<name>`. Without the key, it is the name. The DBIRTH, DDATA, DDEATH and DCMD topics use the `device_id`.
- The longest device topic (`spBv1.0/<group_id>/DBIRTH/<edge_node_id>/<device_id>`) must fit in 159 bytes. With a `group_id` and an `edge_node_id` of 32 bytes, the `device_id` can have 78 bytes.
- The metric names do not get the prefix.

**Name checks for UNS hosts and the UMH preset (UMH-2, UMH-3, #169), in `sparkplug`.**

```json
"sparkplug": { "group_id": "acme", "edge_node_id": "acme:berlin:packaging:gw01",
               "device_prefix": "acme:berlin:packaging:line3", "naming": "umh" }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `naming` | text | `"sparkplug"` | `"sparkplug"` or `"umh"`. `"umh"` selects the UMH preset |

The validation finds the names that UMH Core and other UNS hosts change with no error. Sparkplug permits these names (`tck-id-topic-structure-namespace-valid-device-id`), thus they are warnings. The dry run, the shell `config check`, the boot log and the web UI show them, each with its JSON path.

- **Name rules.**
  - The gateway divides `edge_node_id`, `device_prefix` and each device `name` at `:`. Each level must match `[A-Za-z0-9][A-Za-z0-9_-]*`.
  - The gateway divides each tag `name` at `/`. Each segment must match `[A-Za-z0-9][A-Za-z0-9._-]*`.
  - The warning tells the problem. Examples: an empty level, a space, a character that is not ASCII, `_` at the start, `.` in a level, `:` in a tag name.
- **Other warnings:**
  - a `primary_host_id` that is not empty (UMH publishes a Sparkplug 2.x STATE, #177);
  - a device `type` (UMH gives no value for a template instance);
  - a tag of the type `bytes` (UMH gives no value for Bytes).
- **The UMH preset (`"naming": "umh"`).** The name rules become errors: the validation refuses the configuration, with the JSON path of the first name. The other warnings stay warnings. The default `"sparkplug"` changes nothing for other hosts. Each item of the preset obeys Sparkplug B 3.0.0. The preset also does these items:
  - **Device state (#173).** The NBIRTH has one Boolean metric for each device, `Devices/<name>/Online`, with the alias 80 + the index of the device. Each `:` of the name becomes `/`, because UMH divides a metric name at its last `:` first. An NDATA gives each change: `true` when the device becomes online (before its DBIRTH), `false` when it becomes offline (before its DDEATH). UMH drops null values, the Quality property and the DDEATH. Thus this metric tells UMH that the last values are old. The preset refuses a device name that continues another device name with the level `Online` (`a` and `a:Online`). The reason: the metric of one device would also be a folder.
  - **Plain metrics (#174).** A device with `type` does not publish a template instance. Its DBIRTH and DDATA have one plain metric for each tag, with the tag aliases. The NBIRTH has no template definitions. The gateway does not publish the DataSets `Node Info/Modbus` and `Node Info/Modbus TCP`. In their place, the NBIRTH and the diagnostic NDATA have UInt64 node metrics with the sums of all devices:
    - `Node Info/Modbus/Cycles`, `Failed Cycles`, `OK`, `Timeout`, `CRC`, `Frame` and `Exception` (aliases 60–66, with one or more devices);
    - `Node Info/Modbus TCP/OK`, `Timeout`, `CRC`, `Frame` and `Exception` (aliases 70–74, with `modbus_tcp.enabled`; the sums include the units without a device).

    The state of each device is `Devices/<name>/Online`. A DCMD to a typed device can use the alias of a tag.

**Node diagnostics (SPB-17, MB-13), in `sparkplug`.**

```json
"sparkplug": { "group_id": "Plant1", "edge_node_id": "GW01", "diagnostics_s": 60 }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `diagnostics_s` | integer | `60` | 0 or 10–86400. The period of the NDATA with the diagnostics, in seconds. 0 = only in the NBIRTH |

The node metrics in the NBIRTH and in this periodic NDATA:

| Metric | Alias | Datatype | What it is |
|---|---|---|---|
| `Node Info/Uptime` | 50 | UInt64 | The seconds since the boot |
| `Node Info/Free Heap` | 51 | UInt64 | The free bytes of the internal RAM |
| `Node Info/Min Free Heap` | 52 | UInt64 | The minimum free internal RAM since the boot |
| `Node Info/RSSI` | 53 | Int32 | The dBm of the Wi-Fi access point. Only when the gateway has a Wi-Fi configuration; `null` while it is not associated |
| `Node Info/Modbus` | 54 | DataSet | One row for each device. The columns are `Device` (String), `Online` (Boolean), and `Cycles`, `Failed Cycles`, `OK`, `Timeout`, `CRC`, `Frame`, `Exception` (UInt32). The last three count the attempts, with the retries |
| `Node Info/Modbus TCP` | 56 | DataSet | Only with `modbus_tcp.enabled`. The Modbus TCP requests that the gateway sends to each unit (MB-15). One row for each device: `Device` (String), `Unit`, `Online` (Boolean), `OK`, `Timeout`, `CRC`, `Frame`, `Exception` (UInt32, attempts). A last row `(other units)` (unit 0) is for the units that have no device |
| `Node Info/Largest Free Block` | 58 | UInt64 | The largest free block of the internal RAM, in bytes |
| `Node Info/Free PSRAM` | 59 | UInt64 | The free bytes of the PSRAM |
| `Node Info/Temperature` | 68 | Float | The temperature of the chip, in °C. `null` when there is no reading |

- The Modbus tables go in the NDATA only if a counter or a state changed since the last time (the two tables together). The other metrics of this table go in each diagnostic NDATA.
- The Modbus TCP table is a separate table, not one more column of `Node Info/Modbus`. Thus `Node Info/Modbus` keeps its columns for the hosts that read it. The poll cycles do not apply to the forwarded requests, and the TCP table exists only when the Modbus TCP gateway is on.
- **`Node Info/Time Synced`** (TIME-3, alias 55, Boolean): `true` when SNTP synchronized the clock at least one time since the boot. A time from the RTC at boot (TIME-2) or from the browser (`POST /api/time`) is valid, but it is **not** synchronized. The metric is in the NBIRTH, and in an NDATA of its own each time that it changes (not in the periodic NDATA).
- **`Node Info/Core Dump`** (REL-1, issue #132, alias 57, Boolean): `true` while a core dump is in the `coredump` partition (a panic occurred, and nobody erased the dump). It is in the NBIRTH. When somebody erases the dump (`DELETE /api/coredump` or the shell `coredump erase`), an NDATA of its own sends `false`, with the alias only. The operator downloads the dump with `GET /api/coredump` (§6.1).
- The counters are node metrics, not device metrics. Thus the gateway also publishes the counters of a device that is offline (without a DBIRTH).

**`syslog` (REL-4): a copy of the log to a syslog server.**

```json
"syslog": { "host": "192.168.1.20", "port": 514, "level": "info" }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `host` | text | `""` | The name, IPv4 or IPv6 address of the server. Empty = no remote syslog |
| `port` | integer | `514` | 1–65535, UDP |
| `level` | `error` \| `warning` \| `info` \| `debug` | `info` | The minimum severity that the gateway sends |

The format is RFC 5424 on UDP (RFC 5426), with one message in each datagram. The *facility* is `local0`, HOSTNAME is `edge_node_id`, APP-NAME is the log module, and the message has `[meta sequenceId="n"]`. Example: `<134>1 2026-11-04T12:00:00.123Z GW01 edge - - [meta sequenceId="12"] Sparkplug session up (bdSeq=3)`.

**`web` (CFG-3, CFG-4, CFG-8): the web UI and the REST API.**

```json
"web": { "enabled": true, "port": 80, "username": "admin", "password": "a-long-password" }
```

| Key | Type | Default | Rule |
|---|---|---|---|
| `enabled` | bool | `true` | `false` = no web server with a valid configuration. The server starts in all cases with a rejected configuration, in commissioning, in the fault mode, and after a push of BOOT (NET-5, CFG-8) |
| `port` | integer | `80` | 1–65535, HTTP. With `https` set to `true`, it only redirects (301/308) to HTTPS |
| `https` | bool | `true` | HTTPS with the certificate of the gateway (EC P-256, self-signed). The gateway makes it at the first boot and keeps it with the secrets in NVS. The console shows its SHA-256 fingerprint at each boot. `false` = plain HTTP |
| `https_port` | integer | `443` | 1–65535, not the same as `port` |
| `username` | text | `"admin"` | 1–32 visible ASCII characters, without `:` |
| `password` | text (secret) | `""` | Empty, or 8 to 63 characters, each one printable ASCII (a space to `~`, 0x20 to 0x7E). This is the rule of a WPA2 passphrase: the password is also the WPA2 key of the access point (NET-5, issue #187). Empty = the gateway accepts `admin` with its **commissioning password** (random; the console shows it). While it is empty, a push of BOOT opens the claim window (CFG-8), which sets it without a credential |

**Secrets** (`mqtt.password`, `network.wifi.password`, `web.password`): the gateway keeps them in NVS (CFG-7), not in the JSON. It keeps `web.password` only as a salted PBKDF2-HMAC-SHA256 hash, and as the PMK of the access point (PBKDF2-HMAC-SHA1 with the SSID as the salt, 4096 iterations; issue #187). The gateway calculates the PMK only when the password gets a new value. It removes the PMK with the password. On export, a secret that has a value shows as `"********"`. On import (PUT):

- `"********"` keeps the saved secret;
- another text replaces it (`""` deletes it);
- a key that is not there deletes it: the document replaces the full configuration.

In the file `/cfg/config.json`, a secret that is not there, empty or `"********"` gets the value from NVS. The gateway uses a secret in clear text (in a file that you wrote by hand). Then it moves the secret to NVS and writes the file again without it.

**Limit of attempts:** after 5 failed logins in sequence from one address, that address gets `429` with `Retry-After` for 30 s. During that time, the gateway does not examine the password (also not the correct one). Each new lockout in sequence makes the time two times longer, up to 15 min. A correct login removes the lockout (Kconfig `GW_WEB_LOGIN_*`).

**REST API.** All routes require HTTP Basic with `web.username` and `web.password`, over HTTPS when `https` is on. The only exceptions are the routes of the claim window (CFG-8): `GET /claim`, `GET /api/claim` and `POST /api/claim`. The responses are JSON.

**Writes and CSRF (issue #100).** Each `PUT`, `POST` and `DELETE` must have the header `X-Requested-With` (any value; the web UI sends `gateway`). The client can send `Origin` (or `Referer` when there is no `Origin`). Then it must be this gateway: the scheme of the connection and the `Host` of the request.

- The gateway compares them in a normal form (issue #106). The name is in lower case and without the final dot, IPv6 is between brackets, and the port is decimal. There is no port when it is the port of the scheme (443 with HTTPS, 80 without) or empty.
- Any name that gets to the gateway is correct, if the page and the request use the same name. Examples: the IP, `<hostname>.local`, `<hostname>`, a DNS name of the plant.
- A malformed `Host` or `Origin` is not correct, and `Origin: null` is not correct.
- Otherwise, the response is `403`, before the password check: it does not count as a failed login.

A request can have no credentials, or come from another site. When its body is more than 1 KB, the gateway does not read all of it (issue #106). It sends the response (`401`, `403`, `429`) and closes the connection. Thus a client that continues to send can see a closed connection and not the response.

The gateway does not cut the rejection of an authenticated request from the same origin (`400`, `409`, `413`, `415`). It discards all of the body, and the client receives the response. A client that is not a browser (`curl`, a script) does not send `Origin`: the header is sufficient. For example: `curl -u admin:… -H 'X-Requested-With: curl' -X PUT --data-binary @config.json https://<ip>/api/config`.

| Method and route | What it does |
|---|---|
| `GET /api/config` | Exports the saved configuration, with the secrets masked. `?download=1` sends it as a file |
| `PUT /api/config` (or `POST`) | Imports: validates, saves atomically (a temporary file and `rename`) and restarts in order (NDEATH). `?dry_run=1` only validates. Success: `200 {"ok":true,"saved":false,"warnings":[{"path":"devices[0].name","message":"…"}],"warning_count":1}` (after a save: `"saved":true,"restart":true`). The warnings are the name checks of UMH-2. The response has the first 50, and `warning_count` gives the number of all warnings. Error: `400 {"ok":false,"error":"…","path":"devices[0].tags[1].type","line":0,"column":0}`. `413` above `CONFIG_GW_CONFIG_MAX_BYTES`; `409` during the trial of a new image (issue #144). `?restore_prev=1` (no body): the configuration from before the update replaces the stored one, then the gateway restarts (`404` when there is none, `400` when the firmware rejects it, issue #146) |
| `POST /api/restart` | Restarts in order |
| `POST /api/rebirth` | SPB-9, UMH-7, issue #171: sends the NBIRTH and all the DBIRTH messages again, with the same bdSeq, as the NCMD `Node Control/Rebirth` does. `202 {"ok":true,"rebirth":true}`. `409` when there is no Sparkplug session (not connected, recovery mode, commissioning) |
| `POST /api/modbus/read` | Issue #182: one Modbus read for commissioning, through the task that owns the bus (MB-16), as the shell `mbread` does. Body: `{"unit": 1-247, "area": "coil"\|"discrete"\|"holding"\|"input", "address": 0-65535}`. Optional: `"count"` (1–16, default 1 or the size of the type), `"type"` (a tag type; `bool` only for `coil` and `discrete`; `string` and `bytes` read `count` registers) and `"word_order"` (only with `type`). `200 {"ok":true,"unit":1,"area":"input","address":0,"count":2,"values":[17254,32768],"type":"float32","word_order":"ABCD","value":230.5}`. With the type `int64` or `uint64`, the answer also has `value_text`, the same integer as a text (issue #188). A Modbus error gives `504` (`"result":"timeout"`) or `502` (`crc`, `frame`, or `exception` with the code in `"exception"`), with `error`. `503` when the bus does not run the read in time, or when another test read waits. `400` for a request that is not correct. `409` in recovery mode and in commissioning (the bus does not run). The route only reads |
| `POST /api/time` | TIME-1, issue #160: sets the clock of the gateway and the RTC to the time of the browser. Use it on a network without a time server. Body: `{"utc_ms": <ms since 1970-01-01T00:00:00Z>}`, an integer from 2025 to 2099. `200 {"ok":true,"utc_ms":…,"source":"browser"}`. `400` for a body that is not correct. `409` when SNTP synchronized the clock. The source becomes `browser`, and `Node Info/Time Synced` stays `false`. In normal mode, the gateway then opens its MQTT session if it waited for a valid clock. It also operates in recovery mode and in commissioning |
| `POST /api/firmware` | FW-1: `application/json` with `{"url": "https://…"}` (download) and, as an option, `"force": true`; or `application/octet-stream` with the image as the body (`?force=1` to force). Another `Content-Type` gets `415`. `409` during the trial of a new image (`testing <v>: N s left`) and when the pre-flight check refuses the image (the error lists the reasons; force installs it into recovery mode, issue #144) |
| `POST /api/firmware/other` | Issue #144: the button "Restart into `<version>`". The image of the other slot boots on trial, after the pre-flight check (`?force=1` to force). `202 {"ok":true,"restart":true}`. `409` during a trial or a transfer, or when the pre-flight check refuses; `404` when the other slot is empty or failed its verification |
| `GET /api/firmware` | FW-1: the running slot and the other slot (`other`, `other_slot`, `other_state`: `empty`, `previous`, `new`, `rejected`, `invalid`; `other_why`), `can_boot_other`, `phase`, `state`, `detail`, `progress`, `bytes`, `total`, `trial`, `trial_left_s`, `expected_mode`, `boot_count` and `safe_mode` (issue #144); `cfg_prev` (`version`, `saved`, `schema` of the configuration from before the update; `version` is empty when there is none) and `cfg_prev_restore` (recovery mode: the web UI shows the restore button; issue #146) |
| `POST /api/card/format?confirm=1` | SPB-14, issue #139: starts a format of the microSD card in the background, and returns at once. It deletes everything on the card. Without `?confirm=1`: `400`. `202 {"ok":true,"started":true}` when the format starts. `409` when a format runs. `501` when the build has no card (QEMU) or no PSRAM. `503` when there is no memory for the format task. The errors have `reason`: `busy`, `no_support`, `no_psram` or `no_memory`. A card that does not answer ends the format as `failed` (`card_format` in `/api/status`) |
| `GET /api/coredump` | REL-1, issue #132: downloads the core dump in flash, as it is in the partition (header, ELF and checksum; `application/octet-stream`, `Content-Disposition: attachment`). Decode it with `esp-coredump --core-format raw` and the ELF of the same build. `404` when there is no dump. `409` when the partition holds no readable dump (erase it). **The dump can contain secrets in clear text: keep the file safe** |
| `DELETE /api/coredump` | Issue #132: erases the core dump, after a download or without one. `200 {"ok":true,"erased":true}`. `404` when there is no dump. The gateway then publishes `Node Info/Core Dump` = `false` |
| `GET /api/coredump/summary` | Issue #132: `present`, `state` (`none`, `valid`, `corrupt`), `size` and `reset_reason` (of this boot: the dump can be older). For a valid dump, also: `task`, `pc`, `backtrace` (addresses as `"0x…"` strings), `backtrace_corrupted`, `exc_cause`, `exc_vaddr`, `reason` and `app_elf_sha256`. `reason` is the panic reason, for example `abort() was called at PC 0x… on core 0` (empty when the dump has none). `app_elf_sha256` is the start of the SHA-256 of the ELF that crashed |
| `GET /api/status` | The live state (also in recovery mode, in commissioning and in the fault mode). Refer to the description below |
| `GET /claim` | CFG-8, issue #120: the page of the claim window. No credential. `GET /` without an `Authorization` header goes to it (`303`) while the window is open and the client is a client of the access point |
| `GET /api/claim` | CFG-8: the state of the claim window for this client. No credential. `200 {"open":true,"allowed":true,"left_s":540,"username":"admin"}`. `allowed` is `true` only for a client of the access point. `left_s` and `username` are only for that client. `username` is `""` in recovery mode and in commissioning (any user name) |
| `POST /api/claim` | CFG-8: sets the first web password. No credential, but the CSRF check above and the limit of attempts apply. Body: `{"password": "…"}`: the rule of `web.password` (8 to 63 printable ASCII characters). `200 {"ok":true,"username":"admin","ap":{"ssid":"GW-Setup-A1B2C3","url":"https://192.168.4.1/","secure_in_s":5},"plant":["https://10.0.0.5/"],"mdns":"https://gateway.local/"}`: the password is valid at once, and the window closes. `ap.url` is the address of this connection. 5 s after the response, the access point changes to WPA2 with the new password (issue #187). `plant` has the addresses of the gateway on Ethernet and on the Wi-Fi station. `mdns` is `null` when mDNS is off (NET-6). `403` when a web password is set, when no window is open, or when the client is not a client of the access point. These refusals count as failed logins. `400` for a password or a body that is not correct. `503` when NVS does not store it (the window stays open). The gateway stores only the hash and the PMK, in NVS. The configuration document does not change |
| `POST /api/ap/stop` | NET-5, issue #187: the button "Close the access point". `202 {"ok":true,"stopping":true}`: the access point stops approximately 2 s later, after the response. `409` when the access point does not run. A push of BOOT for 2 to 10 s starts it again |

**`GET /api/status`** sends an object with these items:

- `mode` (`normal`, `recovery`, `commissioning`, `fault`);
- `fault` (issue #135): the hardware fault in the mode `fault`, for example a PSRAM that the boot did not find. Else empty;
- `firmware`, `reset_reason`, `uptime_s`;
- `config` (`ok`, `source`, `error`, `path`);
- `network` (`up`, `interfaces`), `mqtt` (`connected`);
- `sparkplug` (`group_id`, `edge_node_id`, `diagnostics_s`; only with a valid configuration);
- `syslog`, `devices`, `heap`, and the fields of the table below.

For each device, `devices` has `online`, `cycles`, `failed`, `ok`, `timeout`, `crc`, `frame`, `exception` and `tags`. Each tag has `name`, `valid` and `value`. The value is a number, an exact integer for `int64` and `uint64`, a text for `string`, and hexadecimal for `bytes`. An `int64` or `uint64` tag also has `value_text`, the same integer as a text (issue #188). A JSON reader that keeps each number as a double (for example a web browser) rounds an integer above 2^53. The web UI shows `value_text`.

| Field | What it is |
|---|---|
| `io.di`, `io.do` | The bits of the DIs and DOs (bit n = channel n+1). `io.do` is `null` while the expander does not respond |
| `io.do_unknown`, `io.expander` | The expander does not respond; `ready`, `retrying` (retries with backoff) and `attempts` |
| `io.failsafe` | IO-3: the DOs are in their safe state, because there is no Sparkplug session |
| `io.rule_do` | IO-7: the mask of the DOs that a local rule controls (they do not accept NCMD) |
| `io.counters` | IO-4: the 8 pulse counters |
| `buzzer.alarm`, `buzzer.enabled` | IO-6: the alarm that sounds now (`none`, `device_offline`, `mqtt_down`, `config_error`) and the enabled alarms |
| `time.utc_s`, `time.synced`, `time.valid`, `time.source`, `time.waiting` | The time of the gateway (s UTC), and if SNTP synchronized it (TIME-3). `time.valid` tells if the time is valid for timestamps (issue #160: without it, no MQTT session). `time.source` is `sntp`, `rtc` (TIME-2), `browser` (`POST /api/time`) or `none`. `time.waiting` is `true` in normal mode when the MQTT session waits for a valid clock |
| `time.rtc` | `present`, `lost` (`true` when the OS bit was set at boot: the RTC lost the time, examine its battery), the `status` of the last read or write of the RTC, and `stores` (the writes after SNTP or after a time from the browser) |
| `card` (optional, issue #139) | The microSD card. `state` is `unsupported`, `unchecked`, `absent`, `not_mounted`, `mounted`, `formatting` or `formatted`. `unsupported`: the build has no card. `unchecked`: the boot did not try to find a card, because the spill is off. `formatted`: restart to use the card. `detail` tells why, for example `no FAT file system (exFAT, NTFS, ext4, GPT, or no format) (FatFs error 13)`. When known: `name`, `size_mb`, `fat`, `cluster` (B), `free_mb` (with no scan of the FAT) and `other_entries`. `other_entries` are the entries of the root that are not the folder `sf`: the format deletes them too |
| `spill` (optional, normal mode) | The spill of store & forward to the card. `state` is `off`, `not_started`, `on`, `failed` or `stopped`. `off` and `not_started` have `detail`, for example `it needs PSRAM, and none was detected`. `failed` has `detail` and `stranded` (the samples that stay on the card). `stopped`: a format took the card. With `on`, the object also has the counters of the shell `spill` line. They are `ready` (the boot recovery ended), `waiting`, `moved`, `overwritten`, `holes`, `ring_blocks`, `waiting_blocks`, `extents`, `kb`, `blocks_written`, `checkpoints`, `boot_ms` and `files_to_delete` |
| `core_dump` (optional, issue #132) | The core dump in flash: `present`, `state` (`none`; `valid`: the checksum is correct; `corrupt`) and `size` (bytes, 0 = not known). `GET /api/coredump/summary` gives its summary |
| `ap` (optional, issue #187) | The access point (NET-5). `available` (the build has an access point), `simulated` (the model of the QEMU build, which has no radio), `running`, `ssid`, `security` (`open`, `wpa2`; `null` when it does not run), `key` (`web_password` or `commissioning_password`; `null` when it does not run), `clients`, `left_s` (the time to the stop if no client connects or disconnects; `null` when there is no limit now: a client is connected, and there is no maximum time), `open_left_s` (the time left of the open period), `idle_stop_s` and `last_stop` (`none`, `request`, `idle`, `time`) |
| `card_format` (optional, issue #139) | The format of the card. `state`: `idle`, `running` (with `elapsed_s`), `done` (with `duration_s`, `fat`, `cluster`, `size_mb` and `restart: true`) or `failed` (with `duration_s` and `error`) |

A client that does not know the optional fields can ignore them.

## 7. Acceptance criteria of the MVP

1. The `NBIRTH` has `seq` = 0 and the `bdSeq` of the Will. When the TCP connection stops, the broker publishes the `NDEATH` with the same `bdSeq`.
2. When a slave stops, the gateway publishes the DDEATH after `offline_after × poll_ms` (plus the time of the timeouts). When the slave operates again, the gateway publishes the DBIRTH.
3. A change of a DI is in an NDATA in less than 100 ms. An NCMD `DO/3` = true sets output 3 to on, and the NDATA confirms it.
4. A DCMD `Setpoint` = 12.3 writes 123 to the holding register 10 (FC06), and a DDATA confirms it.
5. An NCMD `Node Control/Rebirth` sends the NBIRTH and all the DBIRTHs again, with `seq` set to 0.
6. The payloads decode with no errors with the official `sparkplug_b.proto`, for example with Python and `protobuf`.
