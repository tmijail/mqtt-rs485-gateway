# The architecture of the firmware

This document gives the structure of the firmware: the components, the tasks, the ownership rules and the memory budget (REL-2). For the reasons of the design, refer to [Why the firmware has this architecture](../explanation/architecture.md).

> **Warning: Do not use this document as verified data for the board.** The firmware has never run on the board (#1, #18). The W5500, the RS485 hardware mode of the UART, the TCA9554, the GPIO DIs and the LED are compile-only. The octal PSRAM ran only on the QEMU model. The stack and memory values come from the firmware in QEMU.

## The components

The components are in [`idf/components/`](https://github.com/tmijail/mqtt-rs485-gateway/tree/main/idf/components/). `gw_core` holds all the logic that the host tests examine. The other components are thin adapters around it, on ESP-IDF.

| Component | Function | Uses IDF |
|---|---|---|
| `gw_core` | Pure C logic with no IDF. Refer to [The modules of `gw_core`](#the-modules-of-gw_core) | No. Only [`port/gw_port_esp.c`](https://github.com/tmijail/mqtt-rs485-gateway/blob/main/idf/components/gw_core/port/gw_port_esp.c) uses IDF. The host tests use [`port/gw_port_posix.c`](https://github.com/tmijail/mqtt-rs485-gateway/blob/main/idf/components/gw_core/port/gw_port_posix.c) |
| `nanopb` | The vendored nanopb 0.4.9.1 runtime (Zlib license) | No |
| `gw_rs485` | UART1 in `UART_MODE_RS485_HALF_DUPLEX` (RTS = GPIO21), as an `mb_transport_t` | Yes |
| `gw_bus` | The task that is the **only owner of the RS485 bus**: the polling, the DCMD writes, the Modbus TCP requests and the test reads (MB-16) | Yes |
| `gw_edge` | The Sparkplug task: the esp-mqtt client (TLS, MQTT 5, broker list), bdSeq, the Will, the backoff, the primary host, store & forward, the time limit of each publish, and the `gw_node` engine | Yes |
| `gw_net` | The W5500 on SPI (openeth in QEMU). The optional Wi-Fi STA, as the primary interface or as a backup (NET-4). DHCP or a static IP address for each interface. The optional IPv6, with SLAAC and a static address (NET-7). The default route from `gw_netsel`. SNTP with 1 to 3 servers (#201). The scan of the Wi-Fi networks for the web UI, also with `network.wifi` off (#208) | Yes |
| `gw_bsp` | **The declarative description of the board** ([`gw_bsp.c`](https://github.com/tmijail/mqtt-rs485-gateway/blob/main/idf/components/gw_bsp/gw_bsp.c)): all the pins, the I²C address of the expander and the active levels (DI, DO, RS485 DE, I²C, LED and its color order, buzzer, W5500, microSD). The drivers read it and contain no board data. The RTC address is `GW_PCF85063_ADDR` in [`gw_rtc.h`](https://github.com/tmijail/mqtt-rs485-gateway/blob/main/idf/components/gw_core/include/gw_rtc.h). Refer to [Board data to verify](board-data-to-verify.md) | Yes (types) |
| `gw_board` | The temperature of the chip (`gw_temp.c`, #212; no value in QEMU). The 8 DIs, sampled at 1 kHz, with an inversion and a debounce for each DI (#211) and pulse counters (IO-4, in NVS). Up to 4 DIs are fast counters on a PCNT unit (#218). The 8 DOs on the TCA9554, with the `local_io` task. The WS2812 status LED. The buzzer on GPIO46 (IO-6, LEDC). The BOOT button | Yes |
| `gw_storage` | The configuration from LittleFS, or the embedded configuration when the file is missing. The atomic write of a new configuration. The secrets in encrypted NVS (CFG-7). The network of the last valid configuration, for recovery mode. The factory reset and the commissioning flag (CFG-5). The microSD card of the store & forward spill (`gw_sfcard.c`, #31). The core dump in flash: its state, summary, read and erase (`gw_coredump.c`, #132). The allocation functions of the PSRAM | Yes |
| `gw_web` | The HTTPS server, with a certificate of the gateway, and a redirect from HTTP (#64). The card **Certificates** loads the certificate of the plant, or makes a new self-signed certificate (#205). It also loads, replaces and removes the PEM files (#221). The embedded web UI (CFG-3), the JSON REST API (CFG-4), Basic auth with a limit of the attempts, mDNS and `_https._tcp` (NET-6) | Yes |
| `gw_ap` | The Wi-Fi access point for commissioning (NET-5), separate from `gw_net`. Refer to [The access point](access-point.md). The QEMU build has a simulated access point with the same life and no radio | Yes |
| `gw_fw` | The firmware updates (FW-1): the HTTPS download (`esp_https_ota`) or the upload through the web UI, the pre-flight check, the verification, the self-check of the new image, the rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`), the boot-loop counter, safe mode and the other-slot button (#144). Refer to [Firmware updates](firmware-updates.md) | Yes |
| `gw_log` | The remote syslog (REL-4). A hook on `esp_log_set_vprintf` copies each line to a bounded ring, and a task sends the lines over UDP. The same hook keeps the last lines for the live log of the web UI, from the boot (#200). It sets the log level of a tag at run time (#199) | Yes |
| `gw_mbtcp` | The Modbus TCP gateway (MB-15). A TCP server sends each MBAP request to the bus task and returns the response of the RTU slave. The local unit (`modbus_tcp.local_unit_id`, #216) reads `gw_tagdb`, and its DO writes go to the `local_io` task. Only with `modbus_tcp.enabled` | Yes |
| `gw_shell` | The `esp_console` shell on UART0: `status`, `devices`, `config [show\|load\|check]`, `restart [safe]`, `factory-reset`, `rebirth`, `mbread`, `card`, `ping`, `ap`, `coredump [dump\|erase]`, `identify`, `log`. The commands that change something need `CONFIG_GW_SHELL_WRITE_COMMANDS` (default `y`). The QEMU build also has the emulator commands `di`, `pcnt`, `expander`, `validation`, `storm`, `pubfail`, `boot`, `claimsim`, `apsim` and `panic`. The responses are atomic relative to the log | Yes |
| `main` | `app_main`: loads the configuration and connects the components. The modes are normal, recovery (configuration rejected), commissioning (after a factory reset) and fault (no PSRAM) | Yes |

### The modules of `gw_core`

The source is in [`idf/components/gw_core/src/`](https://github.com/tmijail/mqtt-rs485-gateway/tree/main/idf/components/gw_core/src/), and the host tests are in [`idf/test/host/`](https://github.com/tmijail/mqtt-rs485-gateway/tree/main/idf/test/host/).

| Area | Module | Function |
|---|---|---|
| Modbus | `mb_rtu` | The CRC and the FC01–06, FC15 and FC16 frames |
| | `mb_master` | The transactions, t3.5, the timeout and the retries |
| | `gw_mbap` | The Modbus TCP ↔ RTU frames of the TCP gateway (MB-15) |
| | `gw_mblocal` | The local unit of the Modbus TCP server: the DIs, the DOs, the pulse counters (#216) and the run hours (#234) as Modbus data |
| | `gw_mbdiag` | The request and the answer of the Modbus test read (`POST /api/modbus/read`, #182) |
| Polling and data | `gw_plan` | The block reads |
| | `gw_poll` | The polling, the offline state and the writes |
| | `gw_conv` | The types (up to 64 bits, `string` and register bits), the scale, the byte order and the Sparkplug datatype |
| | `gw_rbe` | Report by exception and the deadband |
| | `gw_devstate`, `gw_backoff` | The state of a device, and the backoff |
| | `gw_tagdb` | The shared process image |
| Sparkplug | `spb`, `sparkplug_b.pb` | The Sparkplug codec, with nanopb |
| | `gw_node` | The Edge Node engine: births, deaths, seq, aliases, NCMD and DCMD, and rebirth |
| | `gw_phost` | The Sparkplug 3.0 STATE of the primary host (SPB-13) |
| | `gw_brokersel` | The broker list, with failover and the return to the primary broker (MQTT-6) |
| | `gw_sf`, `gw_sfspill`, `gw_sfcfg` | The store & forward ring (SPB-14). The policy of the spill to the microSD card: a ring of self-checking 4 KB blocks with a superblock and its boot recovery (#31, #124). The configuration hash of the spill (#145) |
| | `gw_cardfmt` | The state of the microSD card and of its format (#139, #137) |
| | `gw_utf8` | The UTF-8 checks of the text in MQTT topics and Sparkplug strings (#162) |
| Local I/O | `gw_dout` | The boot sequence of the TCA9554 and the I/O polarity |
| | `gw_debounce` | A debounce and an inversion for each DI (#211) |
| | `gw_localio` | The DO policy without a network: the safe state at restart and without MQTT (IO-3), the local DI→DO rules (IO-7) and the pulse DOs (#210). It counts the run hours (#217), and decides when to save the pulse counters and the run hours (IO-4) |
| | `gw_iocmd` | The body of `POST /api/do` (#204) |
| | `gw_buzz` | The buzzer patterns, and the alarms with their delay and priority (IO-6) |
| | `gw_ident` | The time and the pattern of Identify, on the LED and the buzzer (#207) |
| | `gw_button` | The gestures of the BOOT button (CFG-5, NET-5) |
| Configuration | `gw_config` | The parser and the validation of the configuration JSON (`docs/SPEC.md` §6) |
| | `gw_cfglint` | The name checks for UMH Core and other UNS hosts (#169) |
| | `gw_cfgdoc` | The configuration document, with the secrets kept separately: the masked export, the import and the migration. Also a reader for JSON that you paste in the console (CFG-4, CFG-6, CFG-7) |
| | `gw_cfgschema`, `gw_lkgnet` | The "since" schema of the configuration, and the recovery network in a format that accepts new fields (#144) |
| | `gw_cfgprev` | The configuration from before an update: its record, the capture, the restore decision of a downgrade, and the JSON paths that are different (#146) |
| | `gw_certs` | The rules of the certificate files: the limits of the PEM files, the files that the saved configuration names, the plan of a load, the key block of a PEM text, and the hostname in the SANs of the web certificate (#205, #221) |
| Security and access | `gw_auth` | SHA-256, HMAC, PBKDF2 and Basic auth for the web UI (CFG-3) |
| | `gw_authlimit` | A limit of the web login attempts for each address (#64) |
| | `gw_claim` | The claim window for the first web password, and the rule of the access point (CFG-8, #120) |
| | `gw_wpa` | SHA-1, PBKDF2-HMAC-SHA1, the PMK of the access point (the WPA2 key from the web password) and the rule of a WPA2 passphrase (NET-5, #187) |
| | `gw_aplife` | The life of the access point: the stop on request, after the idle time and at the maximum time, and the end of the open period at a claim (NET-5, #187) |
| Network and time | `gw_netsel` | The active interface, Ethernet or Wi-Fi (NET-4), and the "no service" state of an interface (#57) |
| | `gw_wifiown` | The users of the Wi-Fi driver (the station, the access point and a scan), and what the driver must do when a user starts or stops (#208) |
| | `gw_rtc` | The PCF85063 registers ↔ UTC, with the BCD values and the OS bit (TIME-2) |
| | `gw_clock` | The sources of the clock, when the clock is valid, the time from the browser and the hints for the operator (TIME-1, #160) |
| Log and diagnostics | `gw_syslog` | The conversion of log lines to RFC 5424, and a bounded ring (REL-4) |
| | `gw_logring` | The ring of the live log, the tag of a log line, the table of the log levels and the request that changes a level (#199, #200) |
| | `gw_cdump` | The state and the summary of the core dump, and the base64 lines of `coredump dump` (REL-1, #132) |
| Firmware update | `gw_ota` | The trial of a new image (self-check, expected mode), the pre-flight check of an image, the update URLs and the progress (FW-1, #144) |
| | `gw_bootloop`, `gw_fwlast` | The boot-loop counter, and the record of the last update (#144) |
| | `gw_nvsraw` | NVS, encrypted or not encrypted, read from its raw pages (#67) |

### Managed components

Each `idf_component.yml` pins the version with `==`, and `dependencies.lock` has the hash.

- `espressif/mqtt` 1.1.0
- `espressif/w5500` 2.0.1
- `espressif/cjson` 1.7.19~2
- `espressif/led_strip` 3.1.1
- `joltwallet/littlefs` 1.23.1
- `espressif/mdns` 1.14.1

Two components come from third parties: nanopb (a copy in the repository) and `joltwallet/littlefs`. The project does not use the `livekit/nanopb` registry package, because that package has an ABI defect. The project does not use `espressif/esp_io_expander_tca9554` either. That component loads the latch with 0xFF, which is OFF with active-low DOs. But the host tests cannot examine its sequence or its polarity, so `gw_dout` drives the TCA9554.

A change of `littlefs` or of ESP-IDF can change a stored format. Such a change must pass the downgrade test (#148). Refer to [Firmware updates](firmware-updates.md).

## Tasks, queues and ownership

```
                 esp_timer (1 kHz)                 UART0 console (logs + shell)
                 gw_io: debounce DI ──┐
                                      │ flag + wake
  ┌──────────────┐  write queue       ▼            ┌──────────────────────────┐
  │ rs485_bus    │◄── (8 × 64 B) ──── sparkplug ───►│ esp-mqtt (own task)      │
  │ core 1       │                    core 0        │ lwIP / esp_eth (core 0)  │
  │ prio 10      │── dirty bit ─────► prio 8        └──────────────────────────┘
  │ mb_master    │   per device       gw_node        ▲ MQTT_EVENT_DATA → pool of
  │ gw_poller    │── write done ────► (queue 24×4 B) │ 4 command buffers (1 KB)
  └──────┬───────┘                                   │
         │ writes                     reads          │
         ▼                                           │
      gw_tagdb (process image, short mutex) ◄────────┘   status_led (prio 2)
```

"—" in the column **Core** means that the task has no fixed core.

| Task | Core / prio | Stack | Task watchdog (TWDT) | What it owns |
|---|---|---|---|---|
| `rs485_bus` (`gw_bus`) | 1 / 10 | 4 KB static | Yes. The task feeds the TWDT at each Modbus attempt | `mb_master_t`, `gw_poller_t` and UART1. The **only** task that uses the bus (MB-16) |
| `sparkplug` (`gw_edge`) | 0 / 8 | 6 KB static. The E2E requires ≥ 1 KB free after the deepest paths: DCMD on a template member, DCMD with Bytes, NDATA with the DataSet, and the replay of an instance and of bytes | Yes. The task feeds the TWDT after **each publish**, and after each operation on the microSD card (#122). It wakes at least every 1 s, and every 10 ms while the spill has card work (#124) | The MQTT client and `gw_node_t` (seq, aliases, RBE state). All the operations on the microSD card of the spill (`store_forward.sd`), with a maximum of one bounded step in each pass of its loop. All the planned restarts. Refer to [The `sparkplug` task](#the-sparkplug-task) |
| `local_io` (`gw_io`) | — / 9 | 4 KB static | Yes. The task wakes at least every 250 ms, at once on a DI change or a DO command, and at the end of a pulse | The TCA9554 and the PCF85063 RTC on the same I²C bus (TIME-2). Refer to [The `local_io` task](#the-local_io-task) |
| `esp_timer` (IDF) | — | IDF | Yes, as the TWDT user `di_sample`. The sampling callback feeds it every 1 s | The DI sampling (a callback every 1 ms). The time limit of each MQTT publish (`pub_budget`, #59). The time limit of a restart that another task requested (`restart_bound`, 20 s, #136): if the `sparkplug` task did not start the restart, this timer restarts the gateway |
| `status_led` | — / 2 | 2.5 KB | No | The LED (RMT) |
| `console` (`gw_shell`) | — / 2 | 4 KB | No | The console UART (`esp_console` REPL). The task reads `gw_tagdb` and `gw_status`. The writes and the Modbus read go through `gw_storage`, `gw_edge` and the bus queue |
| `syslog` (`gw_log`, only with `syslog.host`) | — / 2 | 3.5 KB static | No | The UDP socket and the consumer side of the ring |
| `modbus_tcp` (`gw_mbtcp`, only with `modbus_tcp.enabled`) | 0 / 3 | 4 KB static | No. All the waits have a limit: `select` 1 s, send 2 s, bus response 8 s | The listen socket, up to 2 clients (`CONFIG_GW_MBTCP_MAX_CLIENTS`) and the one transaction in progress, which `rs485_bus` executes |
| `httpd` (`gw_web`) | — / 3 | 10 KB (HTTPS) or 8 KB (HTTP) | No. The `web_guard` task closes its sessions if a request holds the task for more than `CONFIG_GW_WEB_STALL_S` (#110) | The HTTPS server. The task validates and saves configurations, with buffers in PSRAM. With HTTPS, a second `httpd` task of 3 KB only redirects from HTTP. The shell shows the two tasks as `httpd` and `httpd_redirect`, by their handle, because esp_http_server gives both the same name |
| `fw_update` (`gw_fw`, only during a download or a boot of the other slot) | 0 / 2 | 8 KB from the heap, released at the end | No. Each network wait has a limit of 10 s. The task yields the CPU at each block, and the TWDT watches the IDLE task of its core | The HTTPS download and the write of the passive OTA partition, or the verification of the other slot (#144). A web upload writes from `httpd`. The self-check of a new image runs in the `main` task, which stays until the end of the trial |
| `fw_park`, `fw_install` (`gw_fw`, QEMU build only, `CONFIG_GW_FW_PARK_OTHER_CORE`) | 1 / 23 and 0 / 2 | 2 KB and 6 KB from the heap | No | Only while the app maps the flash for a verification. `fw_park` keeps core 1 in a wait loop. `fw_install` runs that step on core 0 when the caller is on core 1 (#107, #111; refer to [Emulator problems and their workarounds](e2e-harness.md#emulator-problems-and-their-workarounds)) |
| `card_format` (`gw_storage`, only during a format of the microSD card, #139, #137) | 0 / 1 | 5 KB from the internal heap, released at the end | No. The task waits on each SDMMC transfer, so the IDLE task of core 0 runs, and the TWDT watches it | The microSD card during a format. The shell and the web API only start the task. The task writes its free stack in the log at its end. The E2E requires ≥ 1 KB free (#131 measures the real format on the board) |
| `boot_button` (`gw_board`) | — / 2 | 3 KB static | No | GPIO0, sampled every 20 ms. The task requests the confirmation beeps from the buzzer |
| `buzzer` (`gw_board`) | — / 2 | 2.5 KB static | No | GPIO46 and LEDC channel 7 (IO-6). The task reads the `gw_status` flags, selects the alarm (`gw_buzz`), and starts or stops the tone. It runs every 20 ms while the tone sounds, and every 250 ms in silence. A beep request wakes it |
| `mdns` (component) | 0 / 1 | 4 KB | No | The mDNS responder |
| `web_guard` (`gw_web`, #110) | — / 1 | 2.5 KB static | No | The watchdog of the web servers. Every 1 s, it sends a ping to each server. If a request holds a server, it calls `shutdown()` on the sessions of that server |
| esp-mqtt (`mqtt_task`), lwIP (`tcpip`), Ethernet RX (`w5500_tsk` or `emac_rx`), Wi-Fi (only with `network.wifi`), event loop (`sys_evt`) | **0** / IDF | IDF | The TWDT watches the IDLE task | The event loop owns `gw_netsel`: the link and IP events, and the failback timer |

The shell command `status` gives the core of each task (`tasks rs485_bus 1 sparkplug 0 tcpip 0 …`). Its `stack` line gives the minimum free stack of `sparkplug`, `mqtt_task`, `rs485_bus`, `local_io`, `web_guard`, `httpd` and `httpd_redirect`. The E2E examines these values. For the measured values, refer to [Memory thresholds](e2e-harness.md#memory-thresholds).

### The `sparkplug` task

- The task does all the operations on the microSD card of the spill. When a format needs the card (#139), the task stops the spill between two card operations, closes its files and releases the card.
- Before a planned restart, the task moves the RAM ring to the card, in a time limit of 5 s (#136). Refer to [Store & forward](store-and-forward.md).
- The task sends the NCMDs for the DOs to the queue of `local_io` (`gw_io_set_do`, a wait of up to 500 ms). The web server sends the DO commands of the Status tab (`POST /api/do`, #204) to the same queue, with its own answer queue.

### The `local_io` task

- `app_main` sets the DOs to their boot state before the task starts. The task owns the TCA9554 from its start with the valid configuration.
- The task does the DO commands of the NCMDs and of the web UI. It has one queue of 4, and one answer queue for each caller (#204).
- The task does the local DI→DO rules (IO-7). The DI sampling wakes it with a notification.
- The task ends the pulses of the pulse DOs (#210). It reads the PCNT units of the fast counters (#218), and counts the run hours (#217).
- The task sets the safe state after `failsafe_s` with no session (IO-3). It reads `GW_ST_MQTT_UP`.
- The task does the retries for an expander that does not respond.
- The task saves the pulse counters (IO-4) and the run hours (#217) every `counter_save_s`.
- In recovery mode and in commissioning, a reduced form of the task runs. It only does the retries of the expander and the RTC writes (#74).

### Core affinity

All the network stack runs on core 0. The RS485 bus runs on core 1. These settings do this:

- `CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0`;
- `CONFIG_MQTT_USE_CORE_0`;
- `ETH_MAC_FLAG_PIN_TO_CORE` in `gw_net` (`app_main` installs the driver on core 0);
- the default event loop and the Wi-Fi task, which IDF pins to core 0.

The E2E examines the core of each task.

### Concurrency rules

- The producers never block the consumer. The device updates and the DI updates are **atomic flags**, plus an optional wake event. When a wake is lost, no state is lost: the `sparkplug` task reconciles at each loop.
- Data that must not be lost goes through bounded static queues:
  - the commands (a pool of 4 buffers);
  - the write confirmations;
  - the DCMD writes to the bus (a queue of 8).
- The only shared memory is `gw_tagdb`. A mutex protects it, and the code copies it only inside the critical section.
- **All the cJSON operations hold `gw_port_json_lock()` (#106).** `gw_config_parse`, `gw_cfgdoc_apply`, `gw_cfgdoc_export` and `gw_ota_parse_request` hold it for the full operation. The lock is recursive: a static FreeRTOS mutex, or a pthread mutex on the host.
  - This lock is not the lock of `gw_tagdb`. A validation holds it for tens of ms up to some seconds. The polling and the publish do not use it.
  - A host test with two threads fails in each run without the lock.

## The MQTT session

The `sparkplug` task controls the MQTT session. For the keys, refer to [`mqtt`](configuration.md#mqtt) and [The list of brokers](configuration.md#the-list-of-brokers). For the messages, refer to [Sparkplug messages and metrics](sparkplug-messages-and-metrics.md).

- The auto-reconnect function of esp-mqtt is off. The task connects again after an exponential backoff with jitter (1 s to 60 s).
- Before **each** attempt, the task takes the next bdSeq, and sets the matching NDEATH as the Will (`esp_mqtt_set_config`). The bdSeq is in RAM only. It starts at 0 at each boot, and increases by exactly 1 for each MQTT CONNECT packet (SPB-3, #161, `tck-id-topics-nbirth-bdseq-increment`).
  - An attempt that sent no CONNECT packet gives its bdSeq to the next attempt. esp-mqtt sends the CONNECT packet only after the transport (TCP, and TLS) connects. Thus an `MQTT_EVENT_ERROR` while the transport has no socket means no CONNECT. A client that cannot start (no memory) also sent no CONNECT.
  - An attempt that can have sent its CONNECT packet keeps its bdSeq: a CONNECT write that failed, no CONNACK, or a refused CONNACK. The broker can publish the Will of that attempt later (`gw_bdseq_t` in `gw_node`).
- esp-mqtt can report one lost connection two times: a failed write, then the read error. When a retry is already scheduled, the second report is not a new attempt.
- The backoff goes back to the minimum **only after a stable session**: one full keepalive (60 s maximum) after the NBIRTH. A CONNACK alone does not reset the backoff. A failure of `esp_mqtt_client_init` (no memory) waits for the same backoff.
- After the CONNACK, the task sends a SUBSCRIBE (NCMD and DCMD/+), and **waits for its SUBACK** (up to 5 s) before the NBIRTH and the DBIRTHs. In these cases, the task sends DISCONNECT, and tries again with the backoff:
  - the task cannot send the SUBSCRIBE;
  - the broker refuses a topic (code 0x80);
  - the SUBACK does not arrive.

  The broker then discards the Will, because there was no NBIRTH.
- The task does not connect until the clock is valid (#160): no CONNECT, NBIRTH, NDATA or DDATA. The wait has no time limit, and it is not a failed attempt. When the clock becomes valid, the task connects at once. Refer to [The clock and the RTC battery](clock-and-rtc-battery.md).
- **Interface change (NET-4).** With Ethernet and Wi-Fi, the task binds the MQTT socket to the active interface (`SO_BINDTODEVICE`). A session cannot move to a different interface. When `gw_netsel` selects a different interface, the task does these steps:
  1. It publishes the NDEATH. If the old interface does not operate, the NDEATH is lost, and the broker publishes the Will later.
  2. It destroys the client.
  3. It connects a new client through the new interface, with a new bdSeq, an NBIRTH and all the DBIRTHs.

  With only Ethernet, the task does not bind the socket.

## The selection of the interface

`gw_netsel` selects the active interface (NET-4). The event loop owns it. For the keys, refer to [`network.wifi`](configuration.md#networkwifi).

- An interface is *usable* when it has a link (a cable or an association) and an IP address.
- Each interface has a rank. With `role: backup`, Ethernet is the preferred interface. With `role: primary`, Wi-Fi is the preferred interface.
- When no interface is active, the first usable interface becomes active at once, independently of its rank.
- When the active interface is not usable, the best usable interface replaces it at once (failover).
- The preferred interface gets the session back only after it was usable for `failback_s` continuous seconds (failback with hysteresis).
- **No service (#57).** When all the brokers fail on the active interface, the session moves to the other interface. The gateway does not select the failed interface again for 60 s (`CONFIG_GW_NET_NO_SERVICE_MIN_S`). Each new failure in sequence doubles this time, up to 30 min (`CONFIG_GW_NET_NO_SERVICE_MAX_S`). A stable session sets it back to the minimum.
- The default route follows the active interface. DNS and SNTP follow the default route (`CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF`).
- The route priorities of `esp_netif` have the same order: Ethernet 50, and Wi-Fi 20 as a backup or 100 as the primary interface. The default of IDF gives 100 to STA.

## Memory (REL-2)

### Limits

- 16 devices, 64 tags for each device, 256 tags in total.
- **The firmware assumes the PSRAM of the board (#135, decision of the product owner).** The board has 8 MB of octal PSRAM (ESP32-S3-WROOM-1U-N16R8). The firmware does not support a board without PSRAM. Do not add fallbacks or smaller sizes for a board without PSRAM.
- The polling, encoding and publish path does not allocate memory.

### Where the memory goes

All the own memory has a fixed size. It is static, or the firmware allocates it at boot or for one operation.

The functions `gw_storage_malloc`, `gw_storage_calloc` and `gw_storage_big_alloc` allocate in PSRAM. mbedTLS also allocates through them. When the PSRAM is full, the allocation fails: these functions never use internal RAM in its place.

| Item | Where | Size | When |
|---|---|---|---|
| The configuration `gw_config_t` | PSRAM | Approximately 32 KB | At boot |
| The Edge Node engine `gw_node_t` | PSRAM | 21 504 B at most (a `_Static_assert` in [`gw_edge.c`](https://github.com/tmijail/mqtt-rs485-gateway/blob/main/idf/components/gw_edge/gw_edge.c), #80) | At boot |
| The payload buffer | PSRAM | 16 KB (`GW_SPB_PAYLOAD_MAX`) | At boot |
| The store & forward ring (SPB-14, only with `sparkplug.store_forward`) | PSRAM | 40 000 samples of 24 B, 960 KB (`CONFIG_GW_SF_SAMPLES`) | At boot |
| The live log of the web UI (#200), in each mode | PSRAM | 33 KB: the ring of the last log lines (16 KB, `CONFIG_GW_LOG_RING_KB`), the copy buffer of the reader (16 KB), and the table of the 48 log tags (#199) | At boot |
| The spill to the microSD card (only with `store_forward.sd`, #124) | PSRAM | 10 KB: two blocks (one to write, one to read) and the delete queue | At boot |
| The DMA buffer of the SDMMC driver (SDMMC build) | Internal RAM | 4 KB. The driver uses it for each transfer from and to PSRAM (#122) | At boot |
| The FatFs file buffer of the spill | — | 512 B | When the spill opens a file. The files stay open between the operations. The spill opens an extent one time for the writes and one time for the reads. The two superblock files stay open. The 8.3 names need no name buffer (`CONFIG_FATFS_LFN_NONE`) |
| The format of the microSD card (#139, only on request) | Internal DMA-capable RAM | The `card_format` stack (5 KB), a buffer of 16 KB (4 KB when no block of 16 KB is free), the card descriptor and a FatFs volume | During the format. The SDMMC writes need no bounce buffer |
| The state of the card and of the format | Internal `.bss` and `.data` | 268 B (QEMU build), 276 B (SDMMC build) | Static |
| The LittleFS read of the configuration and the cJSON tree | PSRAM | Approximately 200 KB for 256 tags | At boot. Released before the tasks start. Refer to [The configuration document](configuration.md) |
| A configuration validation: the second `gw_config_t`, the request body, the cJSON tree, and one block for the print of the document (#91) | PSRAM | Approximately 32 KB plus the size of the document | During the validation |
| A firmware transfer (FW-1): the download URL, the server CA, the header of an uploaded image, and the `fw_update` stack | PSRAM (the stack from the heap) | — | During the transfer |
| The TLS sessions, the syslog ring, and the buffers of the shell and the web server | PSRAM | — | When in use |
| The FW-1 state data | RTC FAST memory | Approximately 590 B | Static |

The internal `.bss` holds mainly these items:

- `gw_edge`: the 6 KB stack and 4 command buffers of 1.2 KB;
- `gw_bus`: approximately 15 KB (the planner, the polling and a 4 KB stack);
- `gw_core`: approximately 15 KB (`gw_tagdb`, 56 B for each tag);
- `gw_mbtcp`: approximately 6 KB (a 4 KB stack and buffers, also when it is off).

The debug and verbose log lines stay in the image (`CONFIG_LOG_MAXIMUM_LEVEL_VERBOSE`, #199), so that a tag can get more lines at run time. On the image for the board, they add 89 KB of flash and 2 617 B of static internal RAM.

lwIP and esp-mqtt allocate memory internally. Refer to [Known limitations](known-limitations.md).

### The margin of the internal RAM (#80, #91)

The internal RAM is shared with Wi-Fi, lwIP, esp-mqtt, the task stacks and DMA. The E2E measures it in the worst case, in the PSRAM variant (the `tls` scenario, node `GW29`). The worst case is MQTT over TLS 1.3, full births and one open HTTPS session. A second session sends a `PUT /api/config?dry_run=1` of the full document.

| Check | Threshold |
|---|---|
| The minimum free internal RAM since boot (#80) | ≥ 8 KB |
| The largest free internal block at the validation (#80) | ≥ 17 KB |
| The margin after the largest request of the validation print (#91) | ≥ 4 KB |
| The free stack of `sparkplug` after the deepest paths | ≥ 1 KB |

Never lower these thresholds. Measure each change of `.bss`, stack or heap with `E2E_PSRAM=1 --only tls`. For the measured values, refer to [Memory thresholds](e2e-harness.md#memory-thresholds).

The shell gives the current values on each gateway: the `heap` and `stack` lines of `status`. The log of each validation gives `validation: document N B, largest request N B, largest internal block before it N B`.

### The fault mode: no PSRAM (#135)

When the boot does not detect the PSRAM (a defective or missing chip), it continues only to report the fault (`CONFIG_SPIRAM_IGNORE_NOTFOUND`, `GW_WEB_FAULT` in [`main.c`](https://github.com/tmijail/mqtt-rs485-gateway/blob/main/idf/main/main.c)). The gateway does not start: the firmware does not load the configuration, and there is no polling, MQTT or Modbus TCP. All the DOs go off.

| Signal | What it gives |
|---|---|
| The log | `main: PSRAM not detected: the board has 8 MB of PSRAM and the boot did not find it (a defective or missing chip); the gateway does not start (issue #135)`, at boot and every 10 s |
| The shell | `status` gives the line `FAULT    PSRAM not detected: …`. `config` and `mbread` give the same cause |
| The REST API | `GET /api/status` gives `"mode": "fault"` and the cause in `"fault"`. `POST /api/rebirth` and `POST /api/modbus/read` respond `409` with the cause |
| The web UI | The cause in the red banner |
| The LED | The state of a configuration error |

- The network, the HTTPS server, mDNS, the shell and the access point (NET-5) start as in recovery mode, from internal RAM.
- The network uses the settings of the last valid configuration, or DHCP on Ethernet. The web password is the stored one, or the commissioning password.
- A new image on trial (FW-1) goes back to the previous image at once (`no memory to start (no PSRAM)`, #144). A firmware upload through the web UI stays possible.

The E2E scenario `nopsram` boots the PSRAM build on QEMU without the octal PSRAM model.

### The QEMU build without PSRAM (`CONFIG_GW_EMU_NO_PSRAM`)

Only the QEMU build of `sdkconfig.qemu` (`CONFIG_GW_IO_SIMULATED` without `CONFIG_SPIRAM`) runs the gateway without PSRAM. It sets the hidden option `CONFIG_GW_EMU_NO_PSRAM`.

- The allocation functions use internal RAM.
- The payload buffer is 6 KB (`CONFIG_GW_SPB_PAYLOAD_INTERNAL`). The store & forward ring has 512 samples (12 KB). The live log uses rings of 2 KB.
- The spill does not start.
- A birth that does not fit the 6 KB buffer goes to the log, and the gateway does not publish it. Such a message uses no seq number (`tck-id-payloads-sequence-num-incrementing`, #158). A DBIRTH that does not fit leaves its device unborn (#76).
- The E2E uses this build only for the scenarios that do not depend on memory. Refer to [The two variants](e2e-harness.md#the-two-variants-135).

A build without `CONFIG_SPIRAM` and without `CONFIG_GW_IO_SIMULATED` does not compile. `GW_SDKCONFIG_REQUIRED` has `CONFIG_SPIRAM`. Refer to [Build the firmware](../how-to/build-the-firmware.md).
