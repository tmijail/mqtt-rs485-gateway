# The E2E test bench

This document gives the E2E test bench of the firmware: the emulator, the two variants, the programs around the gateway, the result files, the retry rules, the memory thresholds and the limits of QEMU. The checks of each scenario are in [The E2E scenarios](e2e-scenarios.md). To run the tests, refer to [Run the tests](../how-to/run-the-tests.md). For the other test suites and the CI, refer to [Tests](tests.md).

The harness is one file, `idf/tools/e2e/e2e.py`. `idf/tools/e2e/scenarios.py` holds the list of the scenarios, their `--only` names, the variants and the CI groups. `idf/tools/e2e/run_e2e.sh` builds the QEMU images and starts the harness. It passes its extra arguments to `e2e.py`.

## The emulator

- QEMU `esp_develop_9.2.2`, machine `esp32s3`, from `idf_tools.py` (it needs `libslirp0`). The firmware has the `sdkconfig.qemu` overlay (`CONFIG_GW_IO_SIMULATED`).
- Ethernet is `open_eth` on slirp.
- UART0 goes to a socket, for the log and the shell. The harness sets the DIs with the shell command `di`, and the BOOT button with `boot 1` and `boot 0`.
- UART1 goes to a socket and to the pymodbus RTU slave.
- The flash uses DIO, not QIO (refer to [Limits of QEMU](#limits-of-qemu)).
- QEMU runs without `-icount`. Thus 1 s of uptime is 1 s of wall time.

### The two variants (#135)

| Variant | Build | What its full run contains |
|---|---|---|
| PSRAM (`E2E_PSRAM=1`) | `build-qemu-psram`: `sdkconfig.qemu` and `sdkconfig.qemu-psram`. QEMU has the octal PSRAM model (`-global driver=ssi_psram,property=is_octal,value=true`, `-m 8M`) | All the scenarios except the three of the default variant. This is the memory configuration of the board, so each scenario that depends on memory goes here |
| Default | `build-qemu`: `sdkconfig.qemu` only (`CONFIG_GW_EMU_NO_PSRAM`). The own memory is in internal RAM, with a 6 KB payload buffer and a store & forward ring of 512 samples | Only the park of #112, the #97 integration and the #144 self-check (`DEFAULT_VARIANT_SCENARIOS`). They examine logic, not memory, and they are much slower with PSRAM |

`--only` selects scenarios in each variant. The scenario `nopsram` needs the PSRAM build, so the harness skips it in the default variant.

Do not use `-m 32M`. QEMU then maps 31 MB, and no virtual space stays for the map of the flash.

### Other builds

- **The build without NVS encryption (#94).** `run_e2e.sh` also builds `build-qemu-plain` (`sdkconfig.qemu-plain`, without PSRAM in the two variants). It is the firmware before the OTA of `GW42`. `JOBS` sets its parallel jobs. `E2E_PLAIN=0` skips this build, and the harness reports that scenario as `SKIP`.
- **The build with heap poisoning (#104).** `E2E_POISON=1` adds `sdkconfig.qemu-poison` (`build-qemu-poison`). Refer to [Heap poisoning](#heap-poisoning).
- **The earlier release (#148).** `E2E_DOWNGRADE_REF=<git ref>` builds the image of that ref one time into `build-downgrade/<commit>/`, for the `downgrade` scenario.
- The signing key of the test images goes into the build directory (for example `build-qemu/e2e_signing_key.pem`). Never commit it.

### Single-threaded TCG in the OTA scenarios (#107)

The OTA scenarios run with `-accel tcg,thread=single`: the two cores in one host thread. These are the two FW-1 OTA scenarios, the #97 integration, the #144 self-check and the OTA to encrypted NVS. With MTTCG, the QEMU default, the MMU model of the S3 has a race when one core maps flash pages while the other core runs. QEMU then crashes with a SIGSEGV or `Bad ram pointer` when the host load is high (#242). The OTA scenarios map the flash all the time, so they need one thread. The other scenarios keep MTTCG.

With one thread, the guest is slower, and much slower with PSRAM. `E2E_TCG_THREAD` (or `--tcg-thread`) selects `auto` (the default: the behaviour above), `single` or `multi` for all scenarios.

### The self-check time in QEMU (#233)

The QEMU build sets `CONFIG_GW_FW_SELFCHECK_S=30`. The board keeps the default of 60 s.

- Each self-check that the E2E waits for adds its full time.
- The harness reads the value from the `sdkconfig` of the build (`SELFCHECK_S`). It waits for an "installed" state for the self-check time plus 90 s (`SELFCHECK_WAIT_S`).
- The check of a rejection "at once" (#144 item 2) requires less than half of the self-check time (15 s).
- Do not set less than 30 s. During a trial, the E2E pushes the other-slot button, sends an NCMD, saves the configuration and causes a panic. These actions need a sufficient window with PSRAM and single-threaded TCG.
- `cmake/gw_sdkconfig_check.cmake` compares the value with the overlays. Thus a stale `sdkconfig` with 60 s stops the build.

## The programs around the gateway

| Program | What it does |
|---|---|
| pymodbus 3.8.6 RTU slave | Units 1 and 2 respond. Unit 1 (`Meter1`) has a writable float32 `Limit`, the MB-6 tags (`Energy` uint64 ABCD, `Counter` int64 CDAB, `Flow` float64 DCBA, `Temp` int32 BADC, `Serial` string, the register bits `Alarm` and `Ready`), `max_gap: 10`, and `Energy` and `Serial` in a slow scan of 3 s. Unit 2 (`Bulk`) has 60 registers. Units 3 to 5 (`Dead3`–`Dead5`) are in the configuration but never respond. pymodbus is pinned, because version 3.16 changed the datastore API |
| mosquitto 2.0.18 | The main broker. The harness uses a renamed copy of the binary, so that no other process stops it by its name |
| TLS mosquitto (MQTT-4/5) | Four listeners: TLS 1.3 only; TLS 1.2 only with a mandatory client certificate; plain TCP for the observer; TLS 1.3 with an expired broker certificate (#70). The harness makes its own CA (EC P-256), a broker certificate for `IP:10.0.2.2` and `DNS:broker.e2e`, and a client certificate with `openssl`. The PEM files go into the LittleFS partition |
| Brokers with bad behaviour | A minimal MQTT server of the harness. On `--mqtt-port` + 1, it cuts the connection after the CONNACK. On `--mqtt-port` + 2, it answers each SUBSCRIBE with 0x80. mosquitto cannot do this |
| Broker list (MQTT-6) | The primary is a port where nothing listens at first. Then the harness starts and stops a mosquitto there. The backup is the main broker |
| Brokers with a password | The brokers of `GW41` and `GW42` require a user and a password (`mosquitto_passwd`). Thus the NVS migration really tests the MQTT password in the NVS |
| Primary host (SPB-13) | The harness publishes the retained STATE messages (Sparkplug 3.0 JSON) of the host `SCADA1` |
| TCP relay | Between the gateway and the main broker. When the relay closes, the gateway loses the broker, and the observer stays connected. At 700 B/s, the relay simulates a slow link (#59) |
| SNTP responder | On UDP 123 |
| Syslog receiver | UDP, `--syslog-port` (default 15514) |
| Modbus TCP client | A pymodbus client. slirp forwards `--mbtcp-port` (default 15502) to port 502 of the gateway. The connections come from 10.0.2.2, the only address in `allowed_clients` |
| HTTPS clients | slirp forwards `127.0.0.1:18443` to port 443 (`--https-port`) and `127.0.0.1:18080` to port 80 (`--http-port`). The clients do not verify the self-signed certificate. They compare its fingerprint with the fingerprint on the console |
| OTA image servers | An HTTPS server for the images of the FW-1 scenarios, each with a dedicated broker |

The harness writes its own `config.json` into the image under test (`idf/tools/e2e/fs/config.json`: keepalive of 10 s, syslog to the receiver of the harness). For the configuration boots, it makes variants of the image with a different `storage` partition (with `littlefs-python`, the tool of the build) or with the partition erased. It reads the size limit from `CONFIG_GW_CONFIG_MAX_BYTES`. `run_e2e.sh` also passes the ELF and the IDF Python, to decode the core dumps with `esp_coredump`.

The harness asks the shell again until it sees all the expected lines (#56). It sends signals only to the processes that it started.

### Network isolation

The harness moves itself into its own network namespace, with only the loopback interface. This needs `CAP_SYS_ADMIN`, for example root in a container or `docker run --privileged`. Then all ports, also UDP 123, are private to the run, and parallel runs do not collide.

Without this permission, or with `--no-isolate`, the harness uses the host loopback. The ports are `--mqtt-port`, `--modbus-port` and `--console-port` (or `E2E_MQTT_PORT`, `E2E_MODBUS_PORT`, `E2E_CONSOLE_PORT`), with the defaults 18831, 15020 and 15555. If another process uses UDP 123, the TIME-1 check fails, because it requires a response from the SNTP server of the harness.

## The result files

The work directory (`E2E_WORKDIR`) keeps all the files of a run. The harness deletes nothing.

| File | Content |
|---|---|
| `results.json` | `checks`; `durations` of each attempt; `retries` (the discarded attempts, with their passed checks and the fault); `remeasured` (#117); `faults` (scenario, attempt, type, signature, time, PC, analysis), also each firmware fault of the console (type `firmware failure`, #256); `emulators` (command line, pid, load, how it ended); the QEMU version |
| `qemu-exits.txt` | Each QEMU that stopped without a request from the harness, or `none` |
| `uart0-*.log`, `uart0-*.log.qemu` | The console of each boot, and the output of QEMU for that boot. The logs of a discarded attempt stay (append mode) |
| `fault-NN-<log>.txt` | The state of a hung or faulty QEMU: two `info registers -a` outputs, the PC of each core and its region, the last 40 console lines, the stderr of QEMU |
| `qemu/NNN-<log>/` | One directory for each emulator, with core dumps permitted (up to 4 GB). The stderr file starts with the command line, the number of other QEMU processes and the load. If QEMU leaves a core, gdb writes `core-bt.txt` next to it |
| `payloads/` | The captured Sparkplug messages |
| `coredump-N.bin`, `coredump-info-N.txt`, `uart0-panic-N.log` | The core dumps of the `panic` checks |

The CI publishes the logs, `results.json`, `qemu-exits.txt`, the `fault-*.txt` dumps and `core-bt.txt`. It does not publish the QEMU cores.

## Retry rules

The harness makes a distinction between emulator faults and firmware faults (#86, #90, #92, #96). A thread examines each QEMU of the current scenario every half second.

**Only two faults can cause a retry:**

1. QEMU stopped by itself: a signal, an exit code, or a console that closed while the harness used it (`QemuGone`). This applies at any boot of any scenario, also before the harness connects the console.
2. A `LoadStorePIFAddrError` panic with `EXCVADDR` in the peripheral space (0x60000000–0x600FFFFF), during a write or a verification of an app image. The evidence is a line from `esp_https_ota`, `esp_image`, `secure_boot_v2`, the download or the upload, in the 40 lines before. Only the lines after the first `app_main` line count.

A PIF panic with the PC in IRAM and without that line is a firmware fault. The ISR of the RS485 UART and `mask_port()` are also in IRAM and write to peripherals.

The harness repeats the full scenario one time, for a maximum of two scenarios in each run (`MAX_RETRIES`). It does this only when all these conditions are true:

- The first fault is one of the two faults above.
- No check of the attempt failed.
- The console showed no firmware fault. These lines are firmware faults:
  - a `Guru Meditation Error` line with any reason, if it is not the known signature;
  - `assert failed:`, `task_wdt: Aborting`, `Task watchdog got triggered`, `ESP_ERROR_CHECK failed`, `CORRUPT HEAP`, `stack overflow`;
  - in the QEMU build: `fw: core 1 park bound reached`, `fw: core 1 park refused` (#111) and `fw: blocking IPC to core 1 while it was parked` (#112);
  - `abort() was called` or `Backtrace:`, if it is not the `panic` that the harness requested (its marker `panic requested from the console` in the 20 lines before), and not the backtrace of the known signature;
  - `Core dump has been saved to flash`, if the same boot has no `panic` that the harness requested and no known signature (#256). The panic handler takes no console lock, and QEMU does not stall the other core (#238). Thus the panic lines can mix with the lines of the other core, character by character, and the harness finds no `Guru Meditation Error` line. This line of the dump writer comes out complete.

The summary shows the firmware faults also when they cause no failure (`[PANIC]`). `results.json` records each of them in `faults`, with the type `firmware failure`. A firmware fault never causes a retry. Each other case, an expired time limit and a second fault give a failed scenario, with all its checks and the reason for no retry.

**A retry does not hide** a failed check, a hang, or a firmware panic, assert, watchdog, stack overflow or abort on the console. A retry can hide an error that the attempt did not examine yet before QEMU died. A regression that kills QEMU each time still shows, because the second attempt also fails.

**A silence never causes a retry.** A silent QEMU that is alive gives a hang or an expired time limit, and the scenario fails (refer to [Hangs and time limits](#hangs-and-time-limits-92)).

### Hangs and time limits (#92)

- A live QEMU with no UART output for `--hang-timeout` seconds (default 60) is hung, if this occurs where the firmware always prints: a boot (from `ESP-ROM:` to the first `app_main` line) or the signature check of an image. After `Take trusted digest key(s)`, the limit is 30 s.
- Each scenario attempt has a time limit: 900 s by default (`DEFAULT_BUDGET_S`). `SCENARIO_BUDGET_S` gives other limits: 1 800 s for the #97 integration and 3 600 s for `tck`. `--scenario-timeout` sets one limit for all scenarios.
- `--timeout-scale` or `E2E_TIMEOUT_SCALE` multiplies all the limits. Use it on a slow or loaded host. Do not change the checks.
- At a hang or an expired limit, the harness saves the state to `fault-NN-<log>.txt`, kills the emulators of the scenario, and the scenario fails without a retry.
- After a fault, each wait of the harness (MQTT, console, HTTP, syslog) raises `AttemptAborted`, and the scenario ends there. The harness asserts no check on evidence that the fault cut short. Thus a hung QEMU costs less than one minute.
- The monitor holds a lock while it handles an attempt. Thus a fault, or its dump, never goes into the next scenario. If the monitor raises an exception, the run fails.

### What the harness closes after an attempt (#105)

At the end of each attempt, `run_scenario` closes the observers, the relays, the fake brokers and the OTA image servers that the attempt opened, and kills the brokers that it started. It writes `[CLEANUP]` with the list.

- An observer of an attempt that ended adds nothing to the capture. A final check requires that no such message arrived.
- The messages of a discarded attempt go to `DISCARDED_CAPTURE`. The check "6. payloads decode" still decodes them.
- Before each attempt, the harness starts the main broker again if it does not run.
- A retry starts from the same device. The harness saves the image under test and the eFuse file at the start of each scenario, and restores them before the retry.
- After an attempt with a fault or an exception, the harness returns the Modbus slave to its state at the start of that attempt: registers, bits, refused addresses and response delay.

### Upload progress (#115)

A web upload waits while there is progress: the bytes that the gateway accepts (in pieces of 16 KB), and the upload and verification lines on the console. The wait fails after 90 s with no progress, or after 900 s in total. Both scale with `--timeout-scale`.

### Lost RX frames of `open_eth` (#110, #113, #117)

The `open_eth` model of QEMU loses frames when slirp sends bursts (`opencores.emac: RX frame dropped`). If the lost frame is the SYN of a new connection, slirp sends it again only after its exponential wait.

- `sdkconfig.qemu-psram` has 10 DMA receive buffers (`CONFIG_ETH_OPENETH_DMA_RX_BUFFER_NUM=10`). The build without PSRAM keeps 4.
- The web clients of the harness make the TCP connection and the TLS handshake in up to 3 attempts, in the timeout of the caller. They try again only before they send a byte of the request. Thus a request never repeats. `--web-stress` shows the count (`TLS handshake(s) repeated`).
- A lost frame in an open connection comes again after approximately 1 s. The #106 check has a limit of approximately 1 s. The harness measures a case of that check one more time only when the gateway log has an `RX frame dropped` line in the window of that measurement. `results.json` records it in `remeasured`. A failure without a loss in its window, or a second failure, is a failure.

### Heap poisoning

The QEMU build has `CONFIG_HEAP_POISONING_DISABLED`, so freed memory keeps its value. `E2E_POISON=1` builds with `CONFIG_HEAP_POISONING_COMPREHENSIVE`: freed memory gets the value 0xFE, and each block has canaries. Thus a use-after-free, as in the #98 check, changes the reported number of bytes, and the check fails. The CI job `e2e-poison` runs this build two times: `--only ota-fw1` in the PSRAM variant, and `--only ota-park,ota-trial` without PSRAM.

The poisoning headers and canaries use more internal RAM. Without PSRAM, the #80 validation then answers 503, so this build is not in the main variants. For the same reason, `e2e-poison` runs FW-1 in the PSRAM variant: its checks save the configuration, and without PSRAM a save can get 503. `ota-park` and `ota-trial` save no configuration. `ota-wiring` saves the configuration and needs no PSRAM in the full E2E, so `e2e-poison` does not run it.

CAUTION: Read the `uart0-*.log` files when `e2e-poison` fails. With heap poisoning, a `LoadStorePIFAddrError` of the core 1 tick on SYSTIMER can occur at the boot of a trial image, without an image write before it. The harness counts it as a firmware fault, and does not retry it.

### Self-tests of the harness

`--simulate-qemu-death <case>` tests each path.

| Case | What it does | Expected result |
|---|---|---|
| `ota` | SIGABRT to the emulator in the OTA scenario. Also leaves an observer subscribed to the main broker | One retry, pass |
| `boot` | SIGSEGV to the second emulator, 3 s after its start | One retry, pass |
| `console` | SIGSEGV to the first emulator before it connects the console | One retry, pass |
| `main` | SIGABRT in `main` after the replacement of the broker | One retry, pass |
| `hang` | A guest pause with `stop` through the QEMU monitor, after the first `Take trusted digest key(s)` | No retry, fail |
| `hang-mute` | The same hang, with QEMU stopped by SIGSTOP | No retry, fail |
| `ota-failed` | A failed check before the death | No retry, fail |
| `ota-panic` | A firmware `StoreProhibited` panic before the death | No retry, fail |

`idf/tools/e2e/test_harness.py` runs without QEMU, in the `host-tests` job. It examines the known signatures and their negative cases, each firmware fault of the list above, the dump analysis, the retry rule and its limit, the handover between attempts, the #105 cleanup, the return of the Modbus slave, the port of the fake broker, the waits after a fault, the TCG mode, the upload progress wait and the RX frame window. It also examines the CI groups (refer to [Tests](tests.md#the-e2e-groups)). Extend it when you change that logic.

## Memory thresholds

**Never lower these thresholds.** Measure each change of `.bss`, stack or heap with `E2E_PSRAM=1 tools/e2e/run_e2e.sh --only tls`.

| Check | Where | Threshold |
|---|---|---|
| #80 minimum free internal RAM | `tls`, node `GW29`, PSRAM variant | ≥ 8 KB (`RAM_MIN_FREE_B`) |
| #80 largest free internal block at the validation | `tls`, node `GW29` | ≥ 17 KB (`RAM_MIN_BLOCK_B`) |
| #91 spare of the largest block before the document print | `tls`, node `GW29` | ≥ 4 KB (`PRINT_MIN_SPARE_B`) |
| `sparkplug` stack, deepest paths | `sf`, node `GW27` | ≥ 1 024 B (`STACK_MARGIN_B`) |
| `httpd` stack | `tls` and `https` | ≥ 2 048 B (`HTTPD_STACK_MIN_B`) |
| `httpd_redirect` and `web_guard` stacks | `tls` and `https` | ≥ 512 B (`REDIRECT_STACK_MIN_B`) |
| Card format task stack (#139) | `sf`, node `GW23` | ≥ 1 024 B of 5 120 B (`CARD_STACK_MIN_B`) |

The #80 worst case is MQTT over TLS 1.3, full births (the 60-tag DBIRTH of `Bulk`), one HTTPS session open, and `PUT /api/config?dry_run=1` of the full document on a second session. The harness configuration has 5 devices and 74 tags. A larger configuration, or more HTTPS clients at the same time, decrease the margin. mbedTLS, the configuration and the engine are in PSRAM, so the margin of the internal RAM is large on the board configuration.

The harness writes the measured values of a run in the detail of each check, in its output and in `results.json`. The documentation does not record them.

A TLS handshake during a validation needs no fix (#84, node `GW30`). With the dynamic buffers of mbedTLS, a reconnection replaces the earlier session and does not add to it. The web server has one task, so a new HTTPS handshake waits until the PUT ends. A PUT during a handshake, with no 32 KB block free, answers 503 with no hang. The E2E does not force this order.

## Limits of QEMU

### What QEMU does not cover

| Item | What the QEMU build does | What only the board or the host tests examine |
|---|---|---|
| W5500 | `open_eth` | The W5500 driver |
| TCA9554, GPIO, RMT | `CONFIG_GW_IO_SIMULATED`: the DOs are in RAM, the DIs come from the `di` command, the LED is only in the log. The expander registers are in RTC memory, so they keep their values after a software restart or a panic | A real I²C failure (host tests only) |
| TCA9554 absent | `expander absent <ms>` makes the model silent for the first ms of the next boot. The time starts at its first access, because `esp_timer` does not return to 0 at a software restart (#240) | — |
| RS485 mode of the UART | The normal UART mode. `uart_wait_tx_done` causes an assert on `TX_DONE` in QEMU | The RS485 hardware mode |
| Low baud rates | The E2E runs at 9600 baud | The wait for the end of transmission at 1200 baud (host test) |
| Task watchdog | QEMU runs with `wdt_disable` on the timers, so the TWDT cannot trigger. The E2E reads the log to check that the Sparkplug task feeds the TWDT between publish operations | A real trigger |
| Buzzer | The beeps and the alarms are only in the log | LEDC and GPIO46 |
| Wi-Fi | `CONFIG_GW_WIFI=n` and `CONFIG_GW_SOFTAP=n`. A configuration with `network.wifi` boots with a warning and continues on Ethernet. The access point triggers get to `gw_ap`, and `apsim` simulates the access point | The radio. The interface switch by broker reachability (#57) is in host tests only |
| mDNS | The responder starts, but `open_eth` has no multicast filter, so it does not answer | The mDNS answers |
| IPv6 to the host | slirp sends `fec0::2` to `::1` of the host, and the container has no IPv6. The E2E examines only the addresses and ICMPv6 | MQTT over IPv6 |
| Publish budget (#59) | 2 s (`sdkconfig.qemu`) | The 8 s of the board build, and the TWDT trigger |
| PSRAM | The octal model | The time behaviour, the chip detection, the cache with data in transit, and DMA from PSRAM for the SHA and AES accelerators |
| Public CA bundle, microSD card | Not executed. The spill uses a LittleFS directory in place of the card | The real card |

### Emulator problems and their workarounds

Each workaround exists only in the QEMU build or in the harness. On the board, nothing changes.

- **Core dump and the other core (#62, #88).** The panic handler stops the other core with the software stall of `RTC_CNTL`, and QEMU does not emulate it (espressif/qemu#189, #238). The other core continues to run, and the dump can be invalid. The `panic` shell command first parks the other core, then aborts. The check accepts a dump only when `esp_coredump` decodes it and the boot check (`Core dump data checksum is correct`) accepts it. A dump with another task in the dump window, or with `Re-entered core dump`, is an emulator fault: the harness repeats the panic, up to two times. Each other invalid dump fails. `--panics N` repeats the panic N times. A real dump on the board must confirm this (#18).
- **Octal PSRAM, first write of `otadata` (#85).** QEMU crashes with SIGSEGV in `psram_quad_read` when the bootloader writes `otadata` (#239). Before the boot, the harness writes the same `otadata` entry into the image (`write_otadata`), so no boot writes it.
- **Octal PSRAM, GDMA (espressif/qemu#180).** The GDMA does not get to the PSRAM, so the SHA and AES accelerators get no data from mbedTLS buffers in PSRAM. `sdkconfig.qemu-psram` uses software SHA and AES (`CONFIG_MBEDTLS_HARDWARE_SHA=n`, `CONFIG_MBEDTLS_HARDWARE_AES=n`). The board keeps the accelerators.
- **QIO flash.** The reads of the application arrive with an offset of 2 bytes. The QEMU build uses DIO.
- **UART1 after a restart.** The QEMU model does not clear UART1 in `uart_driver_install()`. A response at the time of a restart can keep the interrupt line active, and the next boot stops. `gw_rs485` masks the interrupts of the port before each restart, with a shutdown handler and a wrapper of `esp_restart_noos()` that also covers a panic. The E2E examines this with three panics while a Modbus response is in transit (#65).
- **Cache freeze of the S3 (FW-1).** A map of the flash freezes the caches and does not stop the other core. QEMU does not model the freeze and aborts (`Bad ram pointer`) when the other core runs code from the flash (#241). `CONFIG_GW_FW_PARK_OTHER_CORE` parks core 1 in `waiti` during `esp_ota_begin()` and the image verification. The QEMU build has no secondary USB-Serial-JTAG console, and it installs an uploaded image from a task pinned to core 0.
- **The summary of the core dump (#256).** The summary of a valid dump also maps the flash: the shell command `coredump` and `GET /api/coredump/summary`. In QEMU, the tick of core 0 read SYSTIMER during this map and got `LoadStorePIFAddrError`. The QEMU build thus uses the precaution of the image verification: `gw_fw_run_mapped()` parks core 1 and runs the summary on core 0. The park waits for the bus task, as for an update. The board calls the summary directly.
- **The park of core 1 (#111, #112).** The parked core must hold no lock. Before the park, the bus task stops between two transactions or two attempts (`gw_bus_hold()`), where it holds no lock. The wait is the longest attempt plus 1 s, at least 10 s. If the bus task does not stop, the park does not occur (`fw: core 1 park refused`). A park lasts 45 s at most (`fw: core 1 park bound reached`). The QEMU build wraps `esp_ipc_call()` and `esp_ipc_call_blocking()`, and reports a call to the parked core. The harness counts each of these lines as a firmware failure. A new task pinned to core 1 must stop in the same way as the bus task.
- **QEMU deaths with a loaded host (#107).** With MTTCG, QEMU crashes in the OTA scenarios (refer to [Single-threaded TCG](#single-threaded-tcg-in-the-ota-scenarios-107)). With one thread, QEMU still dies sometimes during an OTA: a SIGSEGV with the PC in `code_gen_buffer`, `iotlb_to_section: Assertion ... failed`, or `atomic_mmu_lookup: code should not be reached` (#243). QEMU can also abort in `main` (MTTCG) with `malloc(): unsorted double linked list corrupted`. The retry covers these faults.
- **Crashes in a row (#144).** The `open_eth` model keeps its state across a panic reset. A second panic with traffic can stop the next boot in an endless `open_eth` interrupt. The E2E uses the emulator command `panic net-off`: it stops the Ethernet driver, then aborts. On the board, a panic resets the W5500 driver.
- **The uptime of the self-check (#144).** `esp_timer` does not restart at a software reset in QEMU. Thus the self-check uses the FreeRTOS tick of the boot.

CAUTION: Do not run two variants or other heavy work at the same time as an E2E run. A high host load makes the real-time checks slower, and QEMU can crash in the OTA scenarios. Do not set `PARALLEL` higher than the number of CPUs divided by 2.

## Other notes

- `idf.py` does not accept `-j`. The build uses the default ninja parallelism. `JOBS` sets the parallel jobs of `run_e2e.sh`.
- `idf/tools/docs_shots.py` makes the screenshots of the web UI with the machinery of the harness (refer to [Make the screenshots of the web UI for the docs](../how-to/make-the-docs-screenshots.md)). The script and the CI do not compare the images, so a change of the web UI does not make a test fail. Some actions of the shots are also browser tests of the web UI (#194, #200, #222). When the page does not obey the rule, the script stops with an error. The CI does not run the script.
