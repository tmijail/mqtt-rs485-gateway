# Tests

This document gives the test suites of the firmware, what each suite examines, the pass criteria and the CI jobs. The E2E tests in QEMU have two documents of their own: [The E2E test bench](e2e-harness.md) (the emulator, the brokers, the variants, the retry rules and the limits of QEMU) and [The E2E scenarios](e2e-scenarios.md) (what each scenario asserts). To run the tests, refer to [Run the tests](../how-to/run-the-tests.md).

> **Warning: The firmware has never run on the board (#1, #18).** All the tests in this document run on the host or in QEMU. The W5500, the RS485 hardware mode, the TCA9554, the GPIO DIs and the LED are compile-only.

## The test levels

| Test | What it examines | Where it runs | Pass criterion |
|---|---|---|---|
| Host tests (QA-1) | The logic of `gw_core` in pure C, without ESP-IDF: the Unity suites, and checks of the `sdkconfig`, the Kconfig names, the web form and the rules of `CLAUDE.md` | `idf/test/host`, CTest | Each CTest test passes. 0 warnings with `-Werror` |
| Host tests at `-O2` | The same tests, with the warnings that only the optimizer gives | CI job `host-tests` | The same |
| Host tests with sanitizers (#225) | The same tests with AddressSanitizer and UndefinedBehaviorSanitizer | CI job `host-tests-sanitizers` | No error from a sanitizer |
| Fuzz tests (#226) | The parsers of `gw_core` with libFuzzer, AddressSanitizer and UndefinedBehaviorSanitizer: the targets in `idf/test/fuzz` | `idf/test/fuzz`, CI jobs `fuzz` (60 s for each target) and `fuzz-weekly.yml` (20 min for each target) | No crash, no sanitizer error, no failed check of a target |
| clang-tidy (#231) | The sources of `gw_core` and of the host tests: the static analyzer of clang, the `bugprone` checks and the warnings of clang | CI job `clang-tidy` | 0 results. Each `NOLINT` comment names its check and gives a reason |
| Sparkplug decode check | The payloads of the engine, decoded with the official Tahu proto | `spb_dump`, `idf/tools/spb_decode.py --check` | 0 structural errors |
| Firmware build (QA-2) | The release build for `esp32s3` | CI job `firmware` | 0 warnings |
| Static analyzer (#230) | The own components (`gw_*`, `main`) of the release build, with the GCC static analyzer (`-fanalyzer`) | CI job `analyzer` | 0 results |
| Linters (#229) | The Python tools, the shell scripts, the workflows and the scripts of the web pages | CI job `host-tests` | 0 results |
| Code scanning (#232) | The flow of data from a source to a `memcpy`, an array index or a length, with CodeQL `security-extended` on the host build | `.github/workflows/codeql.yml` | Each alert in the Security tab is corrected or closed with a reason |
| Documentation site | The links and the anchors of `docs/`, with a build of the site (Zensical `--strict`) | `.github/workflows/docs.yml`, job `docs` | No warning |
| Web UI unit test (#226) | The IPv4 normalization of the Configuration tab, in Node.js without a browser | `idf/test/web/test_ip4norm.mjs`, CI job `host-tests` | All tests pass |
| Install tool unit test (#219) | The logic of the install tool of the documentation site: the access data, the reader of the board and its times, the releases, the manifest and MD5 | `idf/test/web/test_flash_tool.mjs`, CI job `host-tests` | All tests pass |
| Harness self-test | The retry, hang and progress logic of the E2E harness, with fake clocks | `idf/tools/e2e/test_harness.py` | All tests pass |
| Release script self-test (#279) | The manifest of a good build and each refusal of `make_manifest.py`, with a small fake build | `idf/tools/release/test_make_manifest.py`, CI job `host-tests` | All tests pass |
| Site copy self-test (#219) | The copy of the releases to the documentation site and each refusal of `site_releases.py`, with small fake releases | `idf/tools/release/test_site_releases.py`, CI job `host-tests` | All tests pass |
| Release manifest (#279) | The build for the board against `idf/partitions.csv` and its `sdkconfig`: the offsets, the sizes and the options of the build | `idf/tools/release/make_manifest.py`, CI job `firmware` | The script gives no error |
| E2E in QEMU (QA-3) | The real firmware against mosquitto, a pymodbus RTU slave, SNTP, syslog and HTTPS clients | `idf/tools/e2e/run_e2e.sh` | Each check passes in the full run of the three variants |
| Optional E2E scenarios | UMH Core (`umh`), the Sparkplug TCK (`tck`) and the downgrade (`downgrade`) | `run_e2e.sh --only <name>` | Each check passes |

A change is done when the host tests pass and the full E2E passes. The full E2E is the full run of each of the three variants: `E2E_PSRAM=1`, default and `E2E_SECURE=1`.

The PSRAM variant and the default variant run the configuration of the board. The secure variant runs only the scenarios of the NVS encryption and of the signed OTA images (#261). No build for the board has these two functions. Refer to [The variants](e2e-harness.md#the-variants-135-261).

## The results of a run

The documentation does not record the results of a run. The job summary of each CI run gives the results, and the artifacts of the run (Actions tab) hold `results.json`, `qemu-exits.txt` and the logs of each E2E group. A pull request or a commit can also give the results of a local run.

## Host tests

The host tests build `gw_core` and its tests with the host compiler. Platform calls go through `port/gw_port_posix.c`. CMake downloads Unity (v2.6.1) and cJSON (v1.7.19) with `FetchContent`, pinned by commit hash. Thus the first build needs network access.

- A local build without `CMAKE_C_FLAGS` uses `-O0`. The CI job `host-tests` uses `-O2`, because some warnings of `-Wall` and `-Wextra` need the optimizer (for example `-Wformat-truncation` and `-Wstringop-overread`).
- The test files and the host build of `gw_core` use the warning options of `GW_HOST_WARNING_FLAGS` (refer to [Compiler warnings and the stack](compiler-warnings-and-stack.md)). The host build of `gw_core` also uses `-Wconversion`.

### The CTest tests

| Test | What it examines |
|---|---|
| The Unity suites (`test_*`) | Refer to [The Unity suites](#the-unity-suites) |
| `test_sfcfg_packed` (#145) | The cases of `test_sfcfg` again, with `-fpack-struct -fshort-enums` |
| `sfcfg_guard` (#145) | The build stops when a field of the spill hash has no initializer |
| `check_set_config` (#58) | `gw_edge.c` calls `esp_mqtt_set_config()` only through its guarded wrapper `set_config()`, because esp-mqtt 1.1.0 leaks `network.if_name` |
| `check_web_lists` (#167) | The option lists of the configuration form in `index.html` hold each value that `gw_config.c` accepts |
| `check_kconfig_names` and `check_kconfig_names_selftest` (#227) | Each `CONFIG_GW_` name is an option of a `Kconfig` file |
| `check_noalloc` and `check_noalloc_selftest` (#228, REL-2) | No allocation, no cJSON and no JSON lock in the polling, encoding or publish path. Refer to [The rule checks](#the-rule-checks) |
| `check_jsonlock` and `check_jsonlock_selftest` (#228, #106) | No cJSON outside `gw_core`. In `gw_core`, each call path to cJSON passes `gw_port_json_lock()` |
| `check_rs485` and `check_rs485_selftest` (#228) | Only `gw_bus` and `gw_rs485` call the UART and the Modbus master |
| `check_efuse_options` and `check_efuse_options_selftest` (#228, #133) | No option that burns an eFuse in `sdkconfig.defaults` or in an overlay that is not for QEMU |
| The `sdkconfig_check_*` tests (#83, #93, #103, #135) | `cmake/gw_sdkconfig_check.cmake` stops the build when a required option drifts. A stale `sdkconfig` fails, and the `warn` mode only warns. An `sdkconfig` without the PSRAM fails, except with the QEMU overlay |

The cases of the `sdkconfig` check are in [The host tests of the check](build-configuration.md#the-host-tests-of-the-check).

### The rule checks

Four CTest tests examine the rules of `CLAUDE.md` (#228). Each check has a self-test. The self-test gives a small bad example to the check, and the check must refuse it. The bad examples are in `idf/test/host/rules/selftest/` and `idf/test/host/rules/efuse/`. A name with `good_` in the report of a self-test is a false positive, and the self-test fails.

`check_noalloc`, `check_jsonlock` and `check_rs485` use `idf/tools/check_rules.py` and the call graph of the host build. `gw_core` and nanopb compile with `-fcallgraph-info=su`, and GCC writes a `.ci` file next to each object. The script joins these files into one call graph. These three checks need GCC 10 or later, `nm` and Python 3. With another compiler, configure with `-DGW_RULE_CHECKS=OFF`. Then CMake does not add these three checks.

| Check | Rule | How it examines the rule |
|---|---|---|
| `check_noalloc` | REL-2: no allocation in the polling, encoding or publish path | `idf/test/host/rules/noalloc_path.txt` gives the files of the path, each with its reason. `nm -u` examines the object of each file. The call graph examines each function of these files, and each function of `gw_core` and nanopb that they call. A reference to `malloc`, `calloc`, `realloc`, `free`, `strdup`, `cJSON_*` or `gw_port_json_lock` fails. A call of `gw_config_alloc`, `gw_config_release` or `gw_config_json_hooks` also fails, because these functions allocate through a function pointer |
| `check_jsonlock` | #106: each use of cJSON is under `gw_port_json_lock()` | A word `cJSON` in the code of a component other than `gw_core` fails. In `gw_core`, the entries are the functions without a caller and the functions of the public headers. A call path from an entry to a cJSON call must pass a function that calls `gw_port_json_lock()`. A function that calls `gw_port_json_lock()` must also call `gw_port_json_unlock()` |
| `check_rs485` | Only the `rs485_bus` task touches the RS485 UART and the Modbus master | A call of `uart_*`, `mb_master_*` or `gw_rs485_*` in a component other than `gw_bus`, `gw_rs485` and `gw_core` fails. The call graph finds the functions of `gw_core` that reach `mb_master_*`, for example `gw_poller_poll`. A word with their name in another component also fails. A `uart_*` call in `gw_core` fails. `idf/test/host/rules/rs485_exceptions.txt` gives the accepted calls, each with its reason. An exception that the check does not use fails |
| `check_efuse_options` | #133: no build in `main` burns an eFuse | `idf/test/host/check_efuse_options.cmake` reads `sdkconfig.defaults`, its `.<target>` variants and each overlay that is not `sdkconfig.qemu*`. An option of the list in `CLAUDE.md` with the value `y` fails. The QEMU overlays can set these options, because QEMU emulates the eFuses in a file |

The current exception of `check_rs485` is `gw_rs485_init()` in `main/main.c`. The boot installs the UART driver before the `rs485_bus` task exists, and gives the transport to `gw_bus_start()`.

The checks have these limits:

- The call graph has only the direct calls. The checks do not follow a call through a function pointer (`__indirect_call`), for example the callbacks of nanopb, of the bus transport and of the card.
- The call graph does not show a function that is only an argument or a value. For the listed files, `nm -u` finds such a reference to an allocator.
- The call graph is of the host build of `gw_core`. The firmware port `gw_port_esp.c` and the IDF components are not in it. `gw_port_posix.c` is the limit of the graph, because it is not the code of the firmware.
- The checks do not follow the calls into the C library. A `printf` that allocates is not found.
- `check_noalloc` does not examine the IDF adapters of the path (`gw_bus`, `gw_edge`, `gw_board`). They can allocate one time at boot, and a text check cannot see the difference.
- `check_jsonlock` does not examine the order of the calls in one function. A cJSON call before `gw_port_json_lock()` in the same function is not found.
- The text checks of `check_jsonlock` and `check_rs485` remove the comments and the string literals. They do not run the preprocessor, thus a name that a macro makes is not found.
- `check_efuse_options` examines the files of the repository, not the `sdkconfig` of a local build. An option set in `menuconfig` is not found.

### The Unity suites

| Suite | Scope |
|---|---|
| `test_mb_rtu` | CRC (vectors of the Modbus specification), FC01–06/15/16 frames, response length, exceptions, incorrect echo |
| `test_mb_master` | t3.5 and the delay between frames, timeout and retries, bad CRC, incorrect unit, exception without a retry. The wait for the end of transmission is proportional to the frame (FC16 of 13 B at 1200 baud with parity, and the longest FC16). #112 (`test_attempt_bound`): the longest attempt of the slowest valid configuration stays below the 15 s TWDT. A request with a quantity of 0 is refused |
| `test_conv` | MB-6: a scaled uint32 above 2^24 keeps each digit (#165). The `bytes` tag (BADC/DCBA, hexadecimal). The datatype map, also Int64/UInt64/Double/String, and 64-bit scaled values to Double. The orders ABCD/CDAB/BADC/DCBA in 32 and 64 bits, exact 2^53 + 1, INT64_MIN, float64. Strings (fill characters, NUL, non-printable characters, BADC, maximum length). Inverse scale (12.3 → 123), ranges |
| `test_logic` | RBE and deadband against the last reported value (drift), exact 64-bit RBE and string RBE (hash), validity and NaN, debounce, backoff 1–60 s, offline |
| `test_config` | CFG-1 (#162): well-formed UTF-8 in the Sparkplug IDs and names, no two names that differ only in case (`tck-id-case-sensitivity-sparkplug-ids`, `tck-id-case-sensitivity-metric-names`), no metric that is also a folder of another metric. #172: `sparkplug.device_prefix` and the mDNS instance name. #169: the name rules for UNS hosts give warnings with JSON paths, and the UMH preset makes them errors. Each section and its limits: `ota`, `eng_low`/`eng_high`, the `bytes` tag, the device `type`, the parameters of a device type and their values in a device (#276, `test_type_params`: the keys, the limits and each refusal), `sparkplug.diagnostics_s`, `mqtt.tls`, the broker list, `mqtt.protocol`, `sparkplug.store_forward` (#31), `web`, `local_io` (IO-3, IO-4, IO-6, IO-7), `network.hostname`, `mdns`, `syslog`, `network.wifi`, `network.ipv6` (23 invalid IPv6 forms), #226: IPv4 addresses and prefixes in decimal only (no leading zero, sign or space), MB-6, `block_reads`/`max_gap`, `scan`/`slow_poll_ms`, `modbus_tcp`. Errors with a path, line and column, duplicate keys, the maximum nesting, the limits 16/64/256, the maximum document (256 tags, > 40 KB), no available memory. An unknown key longer than the path buffer ends its path with `...` |
| `test_plan` | Block reads, overlaps, gaps, the limit of 125 registers; `block_reads`/`max_gap` per device against the defaults of the build (#38) |
| `test_spb` | NDATA and DDATA without a datatype (#159, `tck-id-payloads-metric-datatype-not-req`). Strings of 256 characters in a round trip; 257 characters are rejected at encode and decode (#87). Round trip of all types, exact 64-bit integers, wire format, NDEATH without seq. SPB-16 properties. SPB-19 Bytes, File and DataSet. SPB-18 template definition and instance. The template parameters (#251, #276): each value field, and the datatypes and the lengths that the encoder refuses. The parameters from a callback of the caller (`test_template_parameters_from_a_callback`). The decode with and without the `param` callback. The worst-case DBIRTH in the buffer, also of a template instance with 8 parameters at their limits (`test_worst_case_template_birth_fits`) |
| `test_node` | The Edge Node engine. SPB-3 (#161, `tck-id-topics-nbirth-bdseq-increment`): the bdSeq starts at 0 at each boot and increases by 1 for each CONNECT. #164: a failed NBIRTH of a Rebirth ends the session. #166 (`tck-id-payloads-name-birth-data-requirement`): each metric has a timestamp. SPB-10 (#163): the datatype of a DCMD. #158 and #76: a DBIRTH that does not fit. #160: no NDATA or DDATA before a valid clock. #172 device prefix. #173/#174 UMH preset. #132 `Node Info/Core Dump`. #144/#223 `Node Control/Boot Other Slot` only with `ota.mqtt`. FW-1 metrics and URLs (#87). SPB-16 to SPB-19, SPB-15 Scan Rate, SPB-17 diagnostics, TIME-3, IO-4 counters. #276 (`test_templates`, `test_template_without_parameters`): a type with a parameter of each type, a device with values and a device without values, and a type without parameters. `test_worst_case_nbirth_fits`: the worst NBIRTH, with 8 parameters at their limits in each of 16 definitions, fits in the payload buffer. Store & forward with `is_historical`. Births, deaths, aliases, the shared seq, rebirth, NCMD and DCMD, discards (#60), "session up" only after the NBIRTH (#71), no DDATA without a DBIRTH (#73) |
| `test_poll` | Polling against a simulated slave: offline after N cycles, exceptions, a block without a response (#66). Counters per device, FC06 write with read-back, FC15/FC16, NaN on coils, out of range. The probe with backoff of offline devices (MB-9, #61). MB-6 decode and writes, MB-12 fast and slow scan, MB-14 a write between requests, SPB-15. Modbus TCP counters (#69) |
| `test_netsel` | NET-4 interface selection: Ethernet only, Wi-Fi as backup or primary, a link that bounces, `failback_s` 0. #57: an interface without a path to the broker |
| `test_brokersel` | MQTT-6 broker list. #156 (`tck-id-operational-behavior-edge-node-termination-host-offline-reconnect`): a valid offline STATE of the primary host moves to the next broker at once |
| `test_phost` | SPB-13: valid STATE payloads, 18 invalid payloads, old timestamps ignored. #157 (`tck-id-operational-behavior-host-application-multi-server-timestamp`): one STATE timestamp for each MQTT server |
| `test_sf` | SPB-14 store & forward and the spill to the microSD card (#124, #136, #142, #143, #152, #248), with a simulated card. Order, overflow, recovery in bounded steps, checkpoints, a maximum of 64 KB of duplicates, configuration changes and a full card. Lost superblocks, damaged blocks and I/O failures (#134). The delete of an extent in bounded steps, and files that are not of the ring (#248). A fault test of 376 restarts (power cut, torn write, lost writes, a damaged write). The move of the RAM ring before a planned restart (#136). |
| `test_sfcfg` | #145: the configuration hash of the spill. A golden value, 30 excluded fields that keep the hash, each field in the hash that changes it |
| `test_authlimit` | #64 login lockout. #110 (`test_webstall`): the stall check of the web server, and the wrap of the 32-bit clock |
| `test_syslog` | REL-4: severity, the line parser, RFC 3339 timestamps, the RFC 5424 format, a full ring without a wait, discards. #232: the hostname read stops at the end of its buffer |
| `test_auth` | SHA-256, HMAC, PBKDF2, the password hash, Basic auth. #100 CSRF check. #106 canonical `Host` and `Origin`, and 17 malformed authorities |
| `test_cfgdoc` | Secrets in the document (mask, delete, migration), errors with a path, masked export and re-import, the Modbus keys after re-import. #187 the PMK of the access point. #91 the print is byte for byte `cJSON_Print`. #106 the JSON lock with two threads |
| `test_button` | BOOT: reset at 10 s, the 2–10 s gesture, a button pressed from boot, bounces, clock wrap |
| `test_mbap` | MB-15 Modbus TCP: MBAP frames, forwarded functions, exceptions 01/03/0A/0B, the client list, the refusal through the access point (#120) |
| `test_dout` | TCA9554: polarity of 8 channels, boot sequence, read-back, bus errors, retry with backoff (#74). IO-3: the latch of `hold` outputs |
| `test_localio` | IO-3 boot state and safe state, IO-4 save schedule, IO-7 rules `follow`, `invert`, `toggle` |
| `test_buzz` | IO-6 beeps, alarms, priority, a silent buzzer |
| `test_rtc` | TIME-2 PCF85063 registers ↔ UTC (each day 2025–2099), the OS bit, invalid fields. #160: the sources of the clock and `POST /api/time` |
| `test_ota` | FW-1 (#144): the expected mode, the self-check, the backstop of 5 min, the rejection of a configuration, the retry of a failed confirmation (#99). The capability block and the pre-flight check (#102, #148). URLs, the request body, the progress |
| `test_cfgschema` | FW-1 (#144 item 3): the schema that a stored configuration needs. #276 (`test_schema3_keys`): a document with a parameter needs schema 3 |
| `test_bootloop` | FW-1 (#144 item 4): the boot-loop counter and safe mode |
| `test_freset` | CFG-5 (#266): the sequence of the factory reset on a model of the two stores. A power cut at each step with each part result of an erase, also in the boots that follow, from each state of the stores. Steps that fail, a full NVS, safe mode, and the boot of a board after its first flash. #280: a blank partition is formatted only when the NVS can hold the commissioning mark, the partition does not mount, and no reset is pending |
| `test_fwlast` | FW-1 (#144 items 5 and 7): the record of the last update |
| `test_lkgnet` | CFG-3 / FW-1 (#144 item 8): the network of the last valid configuration, readable by an earlier or a later image. A field without its terminator |
| `test_claim` | CFG-3 / NET-5 (#120): the claim window and the password rules |
| `test_wpa` | NET-5 (#187): SHA-1, HMAC-SHA1, PBKDF2-HMAC-SHA1, the WPA2 PMK vectors, the SSID. #269: the order of the writes of the hash of the web password and of its PMK, with a cut after each write, and the rule that the access point uses a PMK only with a web password |
| `test_aplife` | NET-5 (#187): the life of the access point, with a fake clock |
| `test_cdump` | REL-1 (#132): base64 of the core dump, the status and summary JSON |
| `test_mbdiag` | #182: `POST /api/modbus/read` |
| `test_nvsraw` | The raw NVS pages: plaintext, encrypted and mixed partitions (#67, #94, #107, #109), the migration copy, the boot decision, `may_erase`, the copy out of reach (#104, #108), FULL and FREEING pages (#97), a lost namespace entry (#185) |
| `test_cardfmt` | #137, #139: the format requests of the card, and their text and JSON |
| `test_cfgprev` | FW-1 (#146): the configuration from before an update, and the downgrade decision. #226: a repeated key in the comparison of two documents |
| `test_ident` | #207: the identify pattern of the LED and the buzzer |
| `test_logring` | #199, #200: the ring of the live log and the log levels |
| `test_mblocal` | #216: the local unit of the Modbus TCP server (DIs, DOs, counters) |
| `test_certs` | #205, #221: the rules of the certificate files |
| `test_wifiown` | #208: the owners of the Wi-Fi driver. #267: the radio as the entropy source never stops the driver, and a station that comes after the STA interface is refused |
| `test_captive` | NET-5 (#274): the captive portal of the access point. The DNS answer: the A record, no records for the other types and classes, and no EDNS option. No answer for a response, another opcode or a question count other than 1. No answer for a compression pointer, a label or name over its limit, a cut question or a query over 512 bytes. The list of probe paths, the Host rule, and the route of a request on the plain HTTP port. The template of the guide page (`www/portal.html`): the values as HTML text, the sections and the errors. The page with and without HTTPS, mDNS and the claim window, with no form and no password field |
| `test_rng` | #267: the order of the two entropy sources at boot. The SAR ADC source goes off before the radio or the temperature sensor starts, and never comes on again after them. The log gives the source of each value, and a warning when no source is on |
| `test_access` | #280: the answer of the shell command `access`. The full answer of a new board, line for line. No commissioning password with a web password set. The cause when there is no URL. Other ports, plain HTTP, more than one interface and the access point. A control character of a value cannot make a line. A buffer that is too small |

### The sanitizer build

The CMake option `GW_SANITIZE` (default `OFF`) builds all the targets with `-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer`. This includes Unity, cJSON, nanopb and `gw_core`. The first error that a sanitizer finds stops the test. LeakSanitizer is on.

- The sanitizer build stays at `-O0`. With the ASan instrumentation, GCC 13 at `-O2` does not give the `-Wformat-truncation` warning.
- `test_sfcfg_packed` has `-fno-sanitize=alignment`, because its structures are packed on purpose (#145). The other checks stay on for this target.
- The CI job sets `vm.mmap_rnd_bits` to 28 first. The ASan runtime of GCC 13 can crash at start with a higher value.

## Fuzz tests

The fuzz tests (#226) give data from outside to the parsers of `gw_core`. Each target in `idf/test/fuzz/` is a libFuzzer program. Clang compiles `gw_core`, nanopb, cJSON and the targets with `-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all`, at `-O2 -g` (without `NDEBUG`). `gw_core` keeps its warning options (`GW_HOST_WARNING_FLAGS` and `-Wconversion`). For clang, `-Wcast-align=strict` becomes `-Wcast-align`. To run the targets, refer to [Run the fuzz targets](../how-to/run-the-tests.md#run-the-fuzz-targets).

| Target | Source of the data | Functions |
|---|---|---|
| `fuzz_mb_rtu` | The RS485 bus | `mb_response_length` byte for byte, `mb_parse_response`, `mb_rsp_reg`, `mb_rsp_bit`, and `mb_master_execute` on a transport that receives the input |
| `fuzz_mbap` | Modbus TCP | The stream of a client as `gw_mbtcp` reads it: `gw_mbap_frame_len`, `gw_mbap_parse`, `gw_mbap_exception`, `gw_mblocal_execute` (the local unit), `gw_mbap_execute` on a transport that receives the input, `gw_mbap_response` |
| `fuzz_spb` | MQTT | `spb_decode_with` with each callback (DataSet columns and cells, template members, template parameters), `spb_rx_number` for each datatype, `spb_rx_prop`. A payload that the validation pass accepts must decode in full with the decoder of `gw_node`, which has no `param` callback |
| `fuzz_node` | MQTT | Up to 8 NCMD, DCMD (also with a topic of the input) and STATE messages for a node in session (`gw_node_handle_message`, `gw_phost_on_state`), with the two configurations of the tests. Each payload that the node publishes must decode |
| `fuzz_config` | The web UI, the REST API, the configuration file | `gw_config_parse` and `gw_config_parse_schema` with an allocator that fails at a selected allocation; on a valid document `gw_config_lint`, the device IDs, the mDNS name, `gw_sfspill_cfg_hash` and the round trip of `gw_lkg_encode` and `gw_lkg_decode`. Also `gw_filerefs_parse` and `gw_json_depth` |
| `fuzz_cfgdoc` | The web UI, the REST API, the shell, the configuration file | `gw_cfgdoc_apply` in both modes, `gw_cfgdoc_export` (no stored secret in the output), `gw_jsonfeed_add` in pieces, `gw_cfgprev_diff` (a document has no difference from itself) |
| `fuzz_web_json` | The REST API, MQTT | `gw_claim_parse`, `gw_mbdiag_parse` and `gw_mbdiag_answer` (the answer must be JSON), `gw_iocmd_parse`, `gw_ota_parse_request` and `gw_ota_url_ok`, `gw_logring_parse_set`, `gw_phost_parse` |
| `fuzz_web_text` | HTTP headers, short fields, uploaded files | `gw_auth_parse_basic`, `gw_base64_decode`, `gw_auth_check_password`, `gw_auth_csrf_check`, `gw_ota_url_ok`, `gw_ota_sha256_parse`, `gw_san_has_host`, `gw_wpa_pass_check`, `gw_wpa_pmk_hex_valid`, `gw_pem_key_blocks`, `gw_config_file_name_ok`, `gw_filerefs_users` |
| `fuzz_flash` | The flash, the RTC chip, a firmware image | `gw_lkg_decode`, `gw_fwlast_parse`, `gw_cfgprev_parse` (each with its round trip), `gw_rtc_decode`, `gw_ota_caps_parse`, `gw_ota_caps_accept`, `gw_ota_preflight` |
| `fuzz_nvs` | The flash, LittleFS | The raw NVS pages (`gw_nvsraw_scan_page`, the u8 lookup, the erase marker) and the copy of a migration (`gw_nvsmig_load`, `gw_nvsmig_check`, `gw_nvsmig_next`, `gw_nvsmig_find`, `gw_nvsmig_orphan_in_copy`). The target can correct the CRCs of the input |
| `fuzz_text` | Many | `gw_utf8_valid` against `gw_utf8_seq`, `gw_parse_ipv6` against `inet_pton`, `gw_hostname_from`, `gw_config_file_name_ok`, `gw_logtag_valid`, `gw_loglevel_parse`. Log lines: `gw_syslog_parse_line`, `gw_syslog_format`, `gw_logring_put`, `gw_logring_copy`, `gw_logring_tag`, `gw_logtags_add` |
| `fuzz_captive` | The clients of the access point (#274) | `gw_captive_dns_answer` with a short output buffer. An answer has the ID, the question and the flags of the query, and at most one A record with the given address. An answer gets no answer itself. `gw_captive_route`, `gw_captive_probe_path` and `gw_captive_host_is_gateway`: the route agrees with the two rules. `gw_captive_render` with a template, a node name and an SSID of the input, and a writer that fails. A value never writes `<`, `>`, `"` or `'` |
| `fuzz_sfspill` | The microSD card | The boot recovery of the spill (`gw_sf_step`, `gw_sfspill_prepare`) on a card image of the input (2 extents of 2 blocks), the replay to a node (`gw_node_flush_history`; each payload must decode), then new blocks on that card. The target can correct the CRCs of the superblocks and of the blocks |

- **Seed corpus.** The build runs `fuzz_seeds`. It writes valid inputs for each target into `<build>/seeds/<target>`. It makes them with the encoders of `gw_core` (Modbus frames, the births and commands of a node, the records, an NVS page, a migration copy, a card that the spill wrote), and from `spec_example.json`, `default_config.json` and `tools/e2e/fs/config.json`. `fuzz_config` and `fuzz_cfgdoc` also get a document with the parameters of a device type (`params_config`, #276). Thus the seeds follow a change of a format.
- **Dictionaries.** `json.dict` has the JSON tokens and the keys and values that `gw_core` reads (for `fuzz_config`, `fuzz_cfgdoc` and `fuzz_web_json`). It also has the keys and the type names of the parameters of a device type (#276). `spb.dict` has the metric names of the commands and the topics (for `fuzz_spb` and `fuzz_node`). Add a new key of the configuration to `json.dict`.
- **Global state.** `fuzz_config` and `fuzz_cfgdoc` set the allocator of the parser (`gw_config_set_allocator`) back to `malloc` and `free` after each input. The parser sets the cJSON hooks again at each call. `fuzz_node` starts each input from a copy of the node after its births, and `fuzz_sfspill` and `fuzz_node` reset `gw_tagdb`.
- **Inputs from outside the gateway.** A buffer with a length goes to the function in a heap block of exactly that length, without a NUL after it (for example a topic, as esp-mqtt gives it). AddressSanitizer then finds a read of one byte past the end.
- **Run.** `idf/test/fuzz/run_fuzz.sh <build> <seconds> [target ...]` runs the targets, one for each CPU at a time. It writes a table with the executions, the coverage and the corpus of each target. A finding goes to `<build>/crashes/` and stops the script with exit code 1.

### Known unstable test

`test_poll` (`test_unanswered_block_fails_the_cycle`) can fail with a difference of 1 ms, for example `Expected …756 Was …757`. The difference is in the timestamp of a tag that the firmware did not read. It seems to depend on the wall clock of the host.

## clang-tidy

The CI job `clang-tidy` runs clang-tidy 18 on the sources of `gw_core` (`src/`, `port/`) and on the host tests (`idf/test/host/`) (#231). The configuration is `idf/.clang-tidy`. A result stops the job. To run it, refer to [Run clang-tidy](../how-to/run-the-tests.md#run-clang-tidy).

- **The compile database.** The job configures the host build with clang and `-DGW_RULE_CHECKS=OFF`, and does not build. Thus `compile_commands.json` has no option that only GCC knows (`-fcallgraph-info=su`).
- **One entry for each source.** clang-tidy 18 examines each entry of a source in one process. A second entry of a source gave false `clang-analyzer-valist.Uninitialized` results. Thus `support.c` is the library `test_support`, and `test_sfcfg_packed` has no entry (`EXPORT_COMPILE_COMMANDS OFF`).
- **The scope.** The headers of `gw_core/include` and of the tests are examined. The generated `sparkplug_b.pb.c` and `.h`, nanopb, cJSON and Unity are not examined.
- **The IDF adapters.** clang-tidy does not examine the other components. `idf.py clang-check` needs the `pyclang` package and the `esp-clang` toolchain, and the `espressif/idf:v6.1` image has neither of them.

The checks are `clang-analyzer-*`, `bugprone-*` and `clang-diagnostic-*` (the warnings of clang with the options of the host build), without the checks of this table. Each of them gave only noise on this code:

| Check that is off | Reason |
|---|---|
| `clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling` | It asks for the functions of C11 Annex K (`memcpy_s`, `snprintf_s`) at each call of `memcpy`, `snprintf` and other standard functions. Newlib and glibc do not have Annex K |
| `clang-analyzer-optin.performance.Padding` | The padding of a structure. Some layouts are part of a format, and the memory budget comes from measurements (REL-2) |
| `bugprone-easily-swappable-parameters` | Two adjacent parameters of the same type. This is a style of the API |
| `bugprone-implicit-widening-of-multiplication-result` | An `unsigned` product that becomes a `size_t`. On the ESP32-S3, `size_t` has 32 bits, and each product is a small index or size |
| `bugprone-narrowing-conversions` | `-Wconversion` of the compiler examines the same. The results were a `char` in a conditional expression with a character constant |
| `bugprone-assignment-in-if-condition` | `if ((p = f()) == NULL)` is a style |
| `bugprone-casting-through-void` | A check for C++. In C, `(T *)(void *)p` tells `-Wcast-align` that the code knows the alignment |
| `clang-analyzer-security.insecureAPI.strcpy` (only in `idf/test/host/.clang-tidy`) | The tests copy literals and values of known length into their own buffers |

A `NOLINT` comment names its check and gives a reason, for example `/* NOLINTNEXTLINE(clang-analyzer-core.NullDereference): cJSON_IsString(NULL) is false */`. The job refuses a `NOLINT` comment without the two. `NOLINTBEGIN` and `NOLINTEND` are permitted for a block (`gw_cfgdoc.c`).

## Sparkplug decode check

`spb_dump` writes real NDEATH, NBIRTH, DBIRTH, DDATA and NDATA payloads of the engine. Each file name is `<n>_<group>+<TYPE>+<edge>[+<device>].bin`: the topic after `spBv1.0/`, with `+` for `/` (#166). `spb_decode.py` compiles the official `sparkplug_b.proto` of eclipse/tahu (an unchanged copy in `idf/components/gw_core/proto/`) with `grpc_tools.protoc`, and decodes the payloads with Google protobuf.

The payloads contain these items: UInt64, Int64, Double, String and register bits; the `Counter/n` counters of IO-4; the SPB-16 properties; `is_null` with BAD quality; a `bytes` tag; DataSets; a File; templates. `--check` applies these rules:

- The structure of the DataSets and of the templates (a definition only in the NBIRTH, without alias).
- The template parameters (#251, #276). Each parameter has a name, a basic datatype and its value in the field of that datatype (`tck-id-payloads-template-parameter-name-required`, `-type-req`, `-type-value`, `-value`).
- A parameter of a birth is String, Int64, Double or Boolean. It has a value, a name of 1 to 39 bytes, and a text of 63 bytes at most. A template of a birth has 8 parameters at most.
- The instance of a DATA message has no parameters.
- Each DBIRTH instance gives each parameter of its definition of the NBIRTH, with the same type (`tck-id-payloads-template-definition-parameters`, `tck-id-payloads-template-instance-parameters`).
- `bdSeq` is Int64 in the NDEATH and the NBIRTH (#154).
- `Node Control/Rebirth` has no alias (#153, `tck-id-operational-behavior-data-commands-rebirth-name-aliases`).
- NDATA and DDATA metrics have no datatype (#159, `tck-id-payloads-metric-datatype-not-req`), and each value is in the field of the datatype of its birth.
- Each metric has a timestamp, also each member of a template instance (#166, `tck-id-payloads-name-birth-data-requirement`).
- The seq increases by 1 after the NBIRTH, 255 followed by 0 (#166, `tck-id-payloads-sequence-num-incrementing`).
- Each alias is unique across the NBIRTH and the DBIRTHs of the node (#166, `tck-id-payloads-alias-uniqueness`).

`idf/tools/test_spb_decode.py` is the self-test of these rules: each rule rejects a payload that breaks it. The E2E also decodes each message that it captures with the same rules (check "6. payloads decode").

## Firmware build

`idf.py set-target esp32s3 build` gives 0 warnings. The QEMU builds (`build-qemu`, `build-qemu-psram`, `build-qemu-psram-secure` and `build-qemu-poison`) also give 0 warnings, but no CI job examines their warnings. The components of the project compile with the options of [Compiler warnings and the stack](compiler-warnings-and-stack.md) (#227).

During the configuration, ESP-IDF v6.1 prints CMake warnings of its own (`component_validation.cmake`: private directories of `esp_wifi` that `wpa_supplicant` uses). They do not come from the project or from the compiler.

## Static analyzer

The CI job `analyzer` builds the release configuration with `-D GW_ANALYZER=ON` in the folder `build-analyzer` (#230). Only the C files of the own components (`gw_*`, `main`) compile with `-fanalyzer`. A result stops the build, because of `-Werror`. ESP-IDF and the managed components compile without the analyzer, from ccache.

- The pass criterion is 0 results. The CI job does not build the QEMU configuration (`CONFIG_GW_IO_SIMULATED`).
- The job runs at the same time as the E2E jobs, so it does not make the CI run longer.

The options and the scope of the analyzer are in [The static analyzer](compiler-warnings-and-stack.md#the-static-analyzer).

## Linters

The `host-tests` job runs the linters (#229). Each linter uses only rules that find errors, and no style rules. A result stops the job.

| Linter | Scope | Configuration |
|---|---|---|
| ruff | The Python files in `idf/tools/` | `idf/tools/ruff.toml`: `F`, `E9`, `PLE`, `B` |
| shellcheck | Each `*.sh` file of the repository | — |
| actionlint | `.github/workflows`, with shellcheck on each `run:` script | — |
| zizmor | The security of the workflows, with `--offline` | Each action is pinned to a full commit hash. `actions/checkout` has `persist-credentials: false` |
| ESLint | The inline `<script>` of `index.html` and `claim.html`, and the modules of the install tool in `docs/flash/` (not `docs/flash/vendor/`) | `idf/tools/lint/eslint.config.mjs`: `eslint:recommended` with the type "problem", `no-redeclare`, `no-global-assign`. Needs Node.js 20.19 or later |

- The versions are pinned. ruff is in `idf/tools/e2e/requirements.txt`. shellcheck, actionlint and zizmor are in `idf/tools/lint/requirements.txt`, as Python wheels with the binary. ESLint is in `idf/tools/lint/package-lock.json`.
- Each `zip()` has a `strict=` argument (B905). Use `strict=False` only for an intended truncation.
- A false positive gets `# noqa: <rule>` or `# shellcheck disable=SCnnnn`, with the reason.
- Do not add an ESLint comment to a web page, because the comment goes into the firmware image. For a false positive, change an option in `eslint.config.mjs`.

## Code scanning with CodeQL

`.github/workflows/codeql.yml` runs GitHub code scanning with CodeQL (#232). CodeQL follows the data from a source (for example a socket, an HTTP request or an MQTT message) to a sink (for example `memcpy`, an array index or a file path). The workflow has two analyses, each in its own job. The results go to the Security tab of the repository as alerts. An alert does not stop the workflow or the CI. To run CodeQL on a computer, refer to [Run CodeQL](../how-to/run-the-tests.md#run-codeql).

| Item | Job `host` | Job `firmware` |
|---|---|---|
| SARIF category | `/language:c-cpp` | `/language:c-cpp/firmware` |
| Runner | `ubuntu-24.04` | `ubuntu-24.04`, in the `espressif/idf:v6.1` image, pinned by digest |
| Build | The host build: `cmake -S idf/test/host -B build-host` and `cmake --build build-host` | The release build (`idf.py set-target esp32s3 build`), with the ccache of the job `firmware` of `ci.yml` (read only). Then CodeQL traces a second `idf.py build`. Before it, the job changes the time of each C file in `idf/components/` and `idf/main/`, and sets `CCACHE_DISABLE=1` |
| Code in the database | `gw_core`, nanopb, Unity, cJSON and the tests | The own components (`gw_*`, `main`) and nanopb. ESP-IDF and the managed components are not in it |
| Sources | The models of CodeQL, for example `fread` and `argv` | The models of CodeQL (for example `recv`, `getaddrinfo` and `fgets`) and the model pack `esp-idf-models` |
| Analysis | `codeql-action/analyze` | The CodeQL CLI of the `init` step: `database finalize`, then `database analyze` with `--additional-packs` and `--model-packs`. `codeql-action/analyze` cannot load a model pack from the repository |
| Artifact (kept 30 days) | `codeql-sarif-host` | `codeql-sarif-firmware` |

Both jobs use the query suite `security-extended` (the job `firmware` without two queries, see below), `github/codeql-action` v4.39.3 and CodeQL 2.27.4. They run on each push to `main`, on each pull request, one time each week (Wednesday 03:41 UTC) and on request.

- Each analysis has its own SARIF category. Thus the alerts of one analysis do not close the alerts of the other.
- The two databases contain `gw_core`. Thus a result in `gw_core` can come from the two analyses, as two alerts.
- Close an alert only with a reason. Use "False positive", "Used in tests" or "Won't fix", and write the reason in the comment.

### The removed results

CodeQL ignores `paths-ignore` for a compiled language. Thus each job filters the SARIF file with `idf/tools/codeql_filter.py` before the upload (`upload-sarif`). The filter keeps only the results in `idf/components/` and `idf/main/`. It removes these results:

- nanopb (`idf/components/nanopb/`) and the generated `idf/components/gw_core/proto/sparkplug_b.pb.c`.
- The test programs (`idf/test/`). This is a decision of the product owner (2026-11-18).
- Unity and cJSON (`build-host/_deps/`).
- The files outside the repository, for example the headers of ESP-IDF.

CodeQL still follows the data through the removed code. The job summary gives the count of the results for each rule.

### Alerts in pull requests

The repository uses the default setting of GitHub for code scanning in pull requests. A new alert with the security severity high or critical makes the check of code scanning fail. A failed check can block the merge of the pull request.

### The model pack `esp-idf-models`

ESP-IDF is not in the database of the job `firmware`. Thus CodeQL does not know where the network data comes into the own code. The model pack `.github/codeql/extensions/esp-idf-models` gives this information (`models/esp-idf.model.yml`). Each model has the kind `remote`, thus `security-extended` uses it.

| Function | Model | The data |
|---|---|---|
| `httpd_req_recv` | Source: argument 1 | The body of an HTTP request |
| `httpd_req_get_hdr_value_len`, `httpd_req_get_url_query_len` | Source: the return value | The length of a header or of the query string |
| `httpd_req_get_hdr_value_str`, `httpd_req_get_url_query_str`, `httpd_req_get_cookie_val` | Source: the output argument | A header, the query string or a cookie |
| `httpd_query_key_value` | Taint from argument 0 to argument 2 | One value of the query string |
| `on_mqtt_data` (`gw_edge.c`) | Source: all parameters | The topic, the payload and the lengths of an MQTT message (`esp_mqtt_event_t`) |
| `gw_node_handle_message` (`gw_node.c`), `on_host_state` (`gw_edge.c`) | Source: the topic, the payload and the lengths | The MQTT message after the queue of the `sparkplug` task |
| `esp_https_ota_get_img_desc`, `esp_https_ota_get_image_size` | Source | The descriptor and the size of the image from the update server |
| `uart_read_bytes` | Source: argument 1 | The responses of the Modbus devices on the RS485 bus |

- A model of C/C++ can name a function, its arguments, its parameters and its return value. It cannot name a read of a field (CodeQL 2.27.4). Thus the fields `content_len` and `uri` of `httpd_req_t` are not sources.
- For the same reason, `on_mqtt_event()` gives the fields of the esp-mqtt event to `on_mqtt_data()` as parameters. The model names these parameters.
- CodeQL does not follow the data through a FreeRTOS queue. Thus the model also names the two functions that get the MQTT message from the queue.
- The model names three own functions: `on_mqtt_data`, `on_host_state` and `gw_node_handle_message`. When you change the name or the parameters of one of them, change the model too. The job `firmware` fails when a function of the model is not in the sources.

### The known false positives

The job `firmware` uses the query suite `.github/codeql/firmware.qls`: `security-extended` without `cpp/world-writable-file-creation` and `cpp/toctou-race-condition` (decision of the product owner, 2026-11-18). These two queries examine file modes, users and other programs, and LittleFS has none of them.

The job can give the alerts of this table. They are false positives. Close each alert one time, with the reason "False positive", and write the reason of the table in the comment. GitHub keeps a closed alert closed in the next runs, unless the code near it changes:

| Rule | Where | Reason |
|---|---|---|
| `cpp/path-injection` | `gw_storage.c`, `gw_storage_read_file()` (`fopen`) | The name comes from the query (`name`, `key_name`) of `PUT /api/files`. `gw_config_file_name_ok()` accepts only the characters A-Z, a-z, 0-9, `.`, `_` and `-`, and no `.` at the start. Thus the path is always a file in `/cfg`. The query `cpp/path-injection` does not know this check |
| `cpp/path-injection` | `gw_storage.c`, `gw_storage_write_file()` (`write_file_atomic`) | The same name. `gw_storage_user_file_name_ok()` makes the same check |

## Web UI unit test

The pages of the web UI go into the firmware image as they are, with their inline `<script>`. `idf/test/web/test_ip4norm.mjs` tests three functions of `index.html` with the test runner of Node.js (`node --test`), without a browser (#226):

- `ip4Norm` removes the spaces around a value, and the leading zeros of each number of a full `a.b.c.d` or `a.b.c.d/n` value. It does not change other values.
- `ip4Kind` gives the fields of the Configuration tab that hold an IPv4 value.
- `ip4Blur` changes the field when it loses the focus, and sends the events `input` and `change`, as a typed change.

The test reads `index.html`, takes the source of each function and runs it in a separate V8 context (`node:vm`). The test also makes sure that the page calls `ip4Blur` on `focusout`. Refer to [`network`](configuration.md#network) for the rule.

## Install tool unit test

The install tool of the documentation site is in `docs/flash/` (#219). Refer to [Flash the firmware from the browser](../how-to/flash-the-firmware-from-the-browser.md). `idf/test/web/test_flash_tool.mjs` imports its modules without the DOM, and tests them with `node --test`:

- `access.js`: the parser of the answer of the command `access`, with the texts of `gw_access_format()`. The test adds the prompt, CR LF, colors and log lines around the answer. It also examines the refusal of an unknown version and of a broken answer.
- `reader.js`: the reader of the board, with a fake clock. "No answer" after 5 s without the start log of this firmware, and at once in the download mode. A wait of up to 30 s with the start log. A late answer, a read again after a restart, and the asks in each state.
- `releases.js`: the list of the releases (only the tags `v0.<minor>.<patch>`, the newest first by number), the checks of the manifest, the value of the fetch option `integrity`, and the notes about the version.
- `md5.js`: the MD5 that esptool-js uses to examine a written part, against `node:crypto`.

The test does not use Web Serial, esptool-js or a board. For a check of the page in a browser, refer to [Build the documentation site](../how-to/build-the-documentation-site.md#examine-the-install-tool).

## CI

`.github/workflows/ci.yml` runs on each push and pull request. The jobs `firmware`, `analyzer`, `e2e-build`, `e2e` and `e2e-poison` run in the `espressif/idf:v6.1` image, pinned by digest. The other jobs run directly on `ubuntu-24.04`.

| Job | What it does |
|---|---|
| `host-tests` | Builds the host tests with `-O2` and runs them. Decodes the `spb_dump` payloads (`spb_decode.py --check`). Runs `test_spb_decode.py`, `test_harness.py`, `test_make_manifest.py`, `test_site_releases.py`, the linters, the unit test of the web UI and the unit test of the install tool |
| `host-tests-sanitizers` | Builds the host tests with `-DGW_SANITIZE=ON` and runs them. Decodes the payloads of the sanitized `spb_dump` |
| `fuzz` | Installs `clang-18` and `libclang-rt-18-dev`, builds `idf/test/fuzz` and runs each target for 60 s, 4 at a time (`run_fuzz.sh`). The start is the corpus of the weekly job (from `actions/cache`, read only) and the seeds. A finding stops the job, and the artifact `fuzz-findings` has the input and the logs. The job runs at the same time as the other jobs |
| `clang-tidy` | Installs `clang-18` and `clang-tidy-18`. Makes sure that each `NOLINT` comment names its check and gives a reason. Configures the host build with clang and runs `run-clang-tidy-18` on `gw_core` and the host tests. A result stops the job |
| `firmware` | Runs `idf.py set-target esp32s3 build`. Fails on a `warning:`. Runs `make_manifest.py` on the build (#279), and fails when the script refuses the build. Publishes the binaries, the release files of the build, the `idf.py size` output and `stack.txt` |
| `analyzer` | Runs `idf.py -B build-analyzer -D GW_ANALYZER=ON set-target esp32s3 build`: the own components with `-fanalyzer` (#230). Fails on a result of the analyzer or on a `warning:`, and gives the results in `components/gw_*` and `main`. The job runs at the same time as the other jobs. Refer to [Static analyzer](#static-analyzer) |
| `e2e-plan` | Reads `CI_GROUPS` from `idf/tools/e2e/scenarios.py`, and gives the matrix of the two next jobs |
| `e2e-build` | One job for each variant (`default`, `psram`, `secure`). Runs `run_e2e.sh` with `E2E_STAGE`, and publishes the images. The `secure` job builds the secure build and the board build `build-qemu-psram`, because the group `nvs-ota` starts on the board build. Only the `secure` job makes a throwaway signing key. GitHub keeps the images for 1 day, because the images of the `secure` job contain the signing key of the run |
| `e2e` | One job for each group of `CI_GROUPS` (5 PSRAM groups, 1 default group and 2 secure groups). Runs `run_e2e.sh --only <group> --no-skip` with `E2E_PREBUILT`. With `--no-skip`, a scenario that shows `[SKIP]` is a failed check. Publishes the logs, the payloads, `results.json`, `qemu-exits.txt` and the `fault-*.txt` dumps |
| `e2e-summary` | Writes one table of all the groups. Fails when a group has no `results.json` or a failed check, or ran other scenarios than its `--only` list. Fails when a group skipped a scenario (`skipped` in `results.json`, #261). Fails when a scenario of a full run ran in no group or in two groups. Also fails when the groups of one variant repeated more scenarios than `MAX_RETRIES` (2) |
| `e2e-poison` | Two jobs: `E2E_POISON=1 run_e2e.sh --no-skip` with `--only ota-fw1` in the PSRAM variant, and with `--only ota-park,ota-trial` without PSRAM, on the build with full heap poisoning (#104). Refer to [Heap poisoning](e2e-harness.md#heap-poisoning) |

`.github/workflows/fuzz-weekly.yml` runs one time each week (Monday 02:17 UTC) and on request (#226). It runs each fuzz target for 20 min, 4 at a time, from the corpus of the previous week. Then it keeps only the inputs that add coverage (`FUZZ_MERGE=1`, `-merge=1` of libFuzzer). It saves the corpus in `actions/cache` (also after a finding) for the next week and for the `fuzz` job, and publishes it as the artifact `fuzz-corpus` (kept 90 days). A finding stops the job, and the artifact `fuzz-weekly-findings` has the input and the logs.

`.github/workflows/codeql.yml` runs CodeQL on the host build (job `host`) and on the firmware build (job `firmware`), on each push to `main`, on each pull request and one time each week (Wednesday 03:41 UTC). Only its two jobs have the permission `security-events: write`. Refer to [Code scanning with CodeQL](#code-scanning-with-codeql).

`.github/workflows/docs.yml` builds the documentation site with Zensical `--strict` on each push and pull request (job `docs`). A link to a page or an anchor that does not exist stops the job. On a push to `main`, the job `docs-deploy` publishes the site to GitHub Pages. Only that job has the permissions `pages: write` and `id-token: write`. Refer to [Build the documentation site](../how-to/build-the-documentation-site.md).

`.github/workflows/release.yml` makes a release of the firmware (#279). It starts on a push of a tag `v0.<minor>.<patch>`, and on request as a dry run. The job `build` does the build for the board with `GW_RELEASE=ON` and runs `make_manifest.py`. For a tag, the job `publish` makes the GitHub pre-release. Only that job has the permission `contents: write`. Refer to [The release workflow](release-manifest.md#the-release-workflow).

The GitHub containers do not permit `unshare`. Thus each `e2e` job runs on the loopback interface of its container.

The `e2e` and `e2e-poison` jobs run in the E2E image `ghcr.io/tmijail/gw-e2e`, pinned by digest (#253). The image is `idf/tools/e2e/Dockerfile`: the pinned ESP-IDF image plus mosquitto, libslirp0, QEMU and the venv of the harness. Thus these jobs install nothing at run time. `.github/workflows/e2e-image.yml` builds and publishes the image when the Dockerfile or `idf/tools/e2e/requirements.txt` changes, and on request. Its job summary gives the digest to pin in `ci.yml`.

The jobs that install Ubuntu packages (`fuzz`, `clang-tidy` and `fuzz-weekly.yml`) use `idf/tools/ci/apt_install.sh`. It does not use a mirror for a package that the runner image has already. For the other packages, it tries the Azure mirror of the GitHub runners first and then the Ubuntu archive, each with retries and a timeout of 30 s. Each `apt-get` command has a time limit, because a mirror can send data too slowly for the timeout. When the limit stops a command, the script tries the next mirror. The step that runs the script has a time limit too.

The workflows pin each action to a commit hash, with the tag in a comment (#229). All the actions run on Node 24 (#235):

| Action | Version |
|---|---|
| `actions/checkout` | v7.0.1 |
| `actions/cache` | v6.1.0 |
| `actions/upload-artifact` | v7.0.2 |
| `actions/download-artifact` | v8.0.2 |
| `actions/upload-pages-artifact` | v5.0.0 |
| `actions/deploy-pages` | v5.0.1 |
| `actions/setup-node` | v6.5.0 |
| `github/codeql-action` (`init`, `analyze`, `upload-sarif`) | v4.39.3 |

### The E2E groups

Each group has its own job, and the groups run at the same time (#121, #135, #233, #261). The groups of a variant are exactly one full run of that variant.

| Variant | Group | `--only` |
|---|---|---|
| PSRAM | `ota-fw1-web` | `ota-fw1-web,nopsram` |
| PSRAM | `ota-fw1-ncmd` | `ota-fw1-ncmd,reset,slow` |
| PSRAM | `main` | `main,https,time` |
| PSRAM | `sf-tls` | `sf,tls,mqtt5,network,failover` |
| PSRAM | `other` | `failsafe,web,phost` |
| Default | `ota-trial` | `ota-park,ota-wiring,ota-trial` |
| Secure | `nvs-ota` | `nvs-ota` |
| Secure | `nvs-signed` | `nvs-boot,signed` |

- The longest group sets the duration of the E2E jobs. `FW-1 OTA by web upload` is the longest scenario, because it has two web uploads of 1 MB with PSRAM.
- The check "6. payloads decode" of each run needs at least one payload. `nopsram` and the #97 integration have no MQTT observer, so each is in a group with a scenario that has one.
- The harness makes some checks one time per run, after the scenarios (refer to [Checks of each run](e2e-scenarios.md#checks-of-each-run)). Thus each group adds these checks. The total line of `e2e-summary` also gives the count as in one full run.
- `test_harness.py` (`CiGroups`) checks that the groups of each variant are one full run, and that each scenario is in one variant and in one group. It checks that no group is empty, and that each group has a scenario with an observer. It checks that a group has only scenarios of its variant, and no scenario that the harness skips in that variant.
- `test_harness.py` (`CiGroups`) also checks the summary. A group that skipped a scenario is a problem, and that scenario does not count as a scenario that ran.
- `test_harness.py` (`SecureVariant`) checks that the harness skips a scenario of the secure variant on another build. It checks that the harness signs a test image only with a signing key, and that a board build never uses an `-unsigned.bin` file. It checks that the arguments agree with the `sdkconfig` of the build.
- If the `psram` jobs are too long, you can set `E2E_TCG_THREAD=multi` in the `e2e` job. Then the OTA scenarios are faster, but QEMU deaths occur, and the harness retries them.

### The caches of the CI

- The jobs `firmware`, `analyzer`, `e2e-build` and `e2e-poison` use ccache (`IDF_CCACHE_ENABLE=1`). `actions/cache` keeps its directory, with one key for each job and variant.
- The job `firmware` of `codeql.yml` reads the cache of the job `firmware` of `ci.yml` (`actions/cache/restore`). It does not save a cache.
- The CI builds in the same directory path in each run. A local build gets the most hits from the cache when it also uses one path.
- `CCACHE_COMPILERCHECK=content` is necessary. Each configuration writes a new `-specs` file into the build directory, so the default check (`mtime`) finds no entry.
- ccache gives the compiler messages again at a hit. Thus the check for 0 warnings of the `firmware` job stays valid.
- The C files of the own components (`gw_*`, `main`) compile without ccache (#227). At a hit, ccache 4.9 does not give the `.ci` file of `-fcallgraph-info` again.
- The `e2e` and `e2e-poison` jobs need no cache for QEMU and the venv of the harness: the E2E image contains them.
