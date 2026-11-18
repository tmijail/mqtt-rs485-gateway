# Tests

This document gives the test suites of the firmware, what each suite examines, the pass criteria, the CI jobs and the current results. The E2E tests in QEMU have two documents of their own: [The E2E test bench](e2e-harness.md) (the emulator, the brokers, the variants, the retry rules and the limits of QEMU) and [The E2E scenarios](e2e-scenarios.md) (what each scenario asserts). To run the tests, refer to [Run the tests](../how-to/run-the-tests.md).

> **Warning: The firmware has never run on the board (#1, #18).** All the results in this document come from host tests and from QEMU. The W5500, the RS485 hardware mode, the TCA9554, the GPIO DIs and the LED are compile-only.

## The test levels

| Test | What it examines | Where it runs | Pass criterion |
|---|---|---|---|
| Host tests (QA-1) | The logic of `gw_core` in pure C, without ESP-IDF: 42 Unity suites, and checks of the `sdkconfig`, the Kconfig names, the web form and the rules of `CLAUDE.md` | `idf/test/host`, CTest | Each CTest test passes. 0 warnings with `-Werror` |
| Host tests at `-O2` | The same tests, with the warnings that only the optimizer gives | CI job `host-tests` | The same |
| Host tests with sanitizers (#225) | The same tests with AddressSanitizer and UndefinedBehaviorSanitizer | CI job `host-tests-sanitizers` | No error from a sanitizer |
| Fuzz tests (#226) | The parsers of `gw_core` with libFuzzer, AddressSanitizer and UndefinedBehaviorSanitizer: 12 targets | `idf/test/fuzz`, CI jobs `fuzz` (60 s for each target) and `fuzz-weekly.yml` (20 min for each target) | No crash, no sanitizer error, no failed check of a target |
| Sparkplug decode check | The payloads of the engine, decoded with the official Tahu proto | `spb_dump`, `idf/tools/spb_decode.py --check` | 0 structural errors |
| Firmware build (QA-2) | The release build for `esp32s3`, and the QEMU builds | CI job `firmware` | 0 warnings |
| Linters (#229) | The Python tools, the shell scripts, the workflows and the scripts of the web pages | CI job `host-tests` | 0 results |
| Harness self-test | The retry, hang and progress logic of the E2E harness, with fake clocks | `idf/tools/e2e/test_harness.py` | All tests pass |
| E2E in QEMU (QA-3) | The real firmware against mosquitto, a pymodbus RTU slave, SNTP, syslog and HTTPS clients | `idf/tools/e2e/run_e2e.sh` | Each check passes in the full run of the two variants |
| Optional E2E scenarios | UMH Core (`umh`), the Sparkplug TCK (`tck`) and the downgrade (`downgrade`) | `run_e2e.sh --only <name>` | Each check passes |

A change is done when the host tests pass and the full E2E passes: the full run of the `E2E_PSRAM=1` variant and the full run of the default variant.

## Current results

| Item | Value |
|---|---|
| CTest tests | 67: 42 Unity suites, `test_sfcfg_packed` and 24 checks |
| Unity cases | 449 in the 42 suites (454 with `test_sfcfg_packed`) |
| Fuzz targets | 12, in `idf/test/fuzz`. Refer to [Fuzz tests](#fuzz-tests) |
| Self-test of the decode rules | 26 tests |
| E2E checks, full run of the PSRAM variant | 264 (284 in the 5 CI groups: each group repeats the 5 checks of the run) |
| E2E checks, full run of the default variant | 18 |
| Optional E2E scenarios | `umh` 15 checks, `tck` 19 checks, `downgrade` 7 checks |

The last local full run was on commit 3b6e3d7, on a cloud container with 4 CPUs (`run_ci_groups.sh`, `PARALLEL=5`, `E2E_TIMEOUT_SCALE=1.5`). The PSRAM variant gave 284/284 checks in the groups, and the default variant gave 18/18. There were no retries and no QEMU exits. The #80 check gave a minimum free internal RAM of 154 868 B and a largest block of 118 784 B at a validation. The host tests gave 59/59 at `-O0`, at `-O2` and with the sanitizers, and the linters gave 0 results. The 8 rule checks of #228 came after that run. With them, the host tests gave 67/67 at `-O0`, at `-O2` and with the sanitizers.

The last CI run was run 44367214752 on commit 3b6e3d7 (2026-11-18). All jobs passed: the host tests, the sanitizers, the linters, the firmware with 0 warnings, the 6 E2E groups (PSRAM 284/284, default 18/18, no retries, no QEMU exits) and the two `e2e-poison` jobs.

The optional scenarios did not run on that commit. Their last results are in [The E2E scenarios](e2e-scenarios.md#optional-scenarios).

## Host tests

The host tests build `gw_core` and its tests with the host compiler. Platform calls go through `port/gw_port_posix.c`. CMake downloads Unity (v2.6.1) and cJSON (v1.7.19) with `FetchContent`, pinned by commit hash. Thus the first build needs network access.

- A local build without `CMAKE_C_FLAGS` uses `-O0`. The CI job `host-tests` uses `-O2`, because some warnings of `-Wall` and `-Wextra` need the optimizer (for example `-Wformat-truncation` and `-Wstringop-overread`).
- The test files and the host build of `gw_core` use the warning options of `GW_HOST_WARNING_FLAGS` (refer to [Compiler warnings and the stack](compiler-warnings-and-stack.md)). The host build of `gw_core` also uses `-Wconversion`.
- `-O3` gives 7 `-Wmaybe-uninitialized` warnings in `gw_config.c`. They are false positives: the analyzer does not see that `fail()` always returns `false`. Issue #231 removes them.

### The CTest tests

| Test | What it examines |
|---|---|
| 42 Unity suites (`test_*`) | Refer to [The Unity suites](#the-unity-suites) |
| `test_sfcfg_packed` (#145) | The 5 cases of `test_sfcfg` again, with `-fpack-struct -fshort-enums` |
| `sfcfg_guard` (#145) | The build stops when a field of the spill hash has no initializer |
| `check_set_config` (#58) | `gw_edge.c` calls `esp_mqtt_set_config()` only through its guarded wrapper `set_config()`, because esp-mqtt 1.1.0 leaks `network.if_name` |
| `check_web_lists` (#167) | The option lists of the configuration form in `index.html` hold each value that `gw_config.c` accepts |
| `check_kconfig_names` and `check_kconfig_names_selftest` (#227) | Each `CONFIG_GW_` name is an option of a `Kconfig` file |
| `check_noalloc` and `check_noalloc_selftest` (#228, REL-2) | No allocation, no cJSON and no JSON lock in the polling, encoding or publish path. Refer to [The rule checks](#the-rule-checks) |
| `check_jsonlock` and `check_jsonlock_selftest` (#228, #106) | No cJSON outside `gw_core`. In `gw_core`, each call path to cJSON passes `gw_port_json_lock()` |
| `check_rs485` and `check_rs485_selftest` (#228) | Only `gw_bus` and `gw_rs485` call the UART and the Modbus master |
| `check_efuse_options` and `check_efuse_options_selftest` (#228, #133) | No option that burns an eFuse in `sdkconfig.defaults` or in an overlay that is not for QEMU |
| 11 `sdkconfig_check_*` tests (#83, #93, #103, #135) | `cmake/gw_sdkconfig_check.cmake` stops the build when a required option drifts. A stale `sdkconfig` fails, and the `warn` mode only warns. An `sdkconfig` without the PSRAM fails, except with the QEMU overlay |

The `sdkconfig` check (#83) covers only the options in `GW_SDKCONFIG_REQUIRED`. If the firmware starts to depend on a new option, add it to that list. An intentional change of one of these options in `menuconfig` also stops the build: use `-D GW_SDKCONFIG_CHECK=warn`.

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

| Suite | Cases | Scope |
|---|---|---|
| `test_mb_rtu` | 6 | CRC (vectors of the Modbus specification), FC01–06/15/16 frames, response length, exceptions, incorrect echo |
| `test_mb_master` | 10 | t3.5 and the delay between frames, timeout and retries, bad CRC, incorrect unit, exception without a retry. The wait for the end of transmission is proportional to the frame (FC16 of 13 B at 1200 baud with parity, and the longest FC16). #112 (`test_attempt_bound`): the longest attempt of the slowest valid configuration stays below the 15 s TWDT. A request with a quantity of 0 is refused |
| `test_conv` | 13 | MB-6: a scaled uint32 above 2^24 keeps each digit (#165). The `bytes` tag (BADC/DCBA, hexadecimal). The datatype map, also Int64/UInt64/Double/String, and 64-bit scaled values to Double. The orders ABCD/CDAB/BADC/DCBA in 32 and 64 bits, exact 2^53 + 1, INT64_MIN, float64. Strings (fill characters, NUL, non-printable characters, BADC, maximum length). Inverse scale (12.3 → 123), ranges |
| `test_logic` | 8 | RBE and deadband against the last reported value (drift), exact 64-bit RBE and string RBE (hash), validity and NaN, debounce, backoff 1–60 s, offline |
| `test_config` | 47 | CFG-1 (#162): well-formed UTF-8 in the Sparkplug IDs and names, no two names that differ only in case (`tck-id-case-sensitivity-sparkplug-ids`, `tck-id-case-sensitivity-metric-names`), no metric that is also a folder of another metric. #172: `sparkplug.device_prefix` and the mDNS instance name. #169: the name rules for UNS hosts give warnings with JSON paths, and the UMH preset makes them errors. Each section and its limits: `ota`, `eng_low`/`eng_high`, the `bytes` tag, the device `type`, `sparkplug.diagnostics_s`, `mqtt.tls`, the broker list, `mqtt.protocol`, `sparkplug.store_forward` (#31), `web`, `local_io` (IO-3, IO-4, IO-6, IO-7), `network.hostname`, `mdns`, `syslog`, `network.wifi`, `network.ipv6` (23 invalid IPv6 forms), #226: IPv4 addresses and prefixes in decimal only (no leading zero, sign or space), MB-6, `block_reads`/`max_gap`, `scan`/`slow_poll_ms`, `modbus_tcp`. Errors with a path, line and column, duplicate keys, the maximum nesting, the limits 16/64/256, the maximum document (256 tags, > 40 KB), no available memory. An unknown key longer than the path buffer ends its path with `...` |
| `test_plan` | 3 | Block reads, overlaps, gaps, the limit of 125 registers; `block_reads`/`max_gap` per device against the defaults of the build (#38) |
| `test_spb` | 15 | NDATA and DDATA without a datatype (#159, `tck-id-payloads-metric-datatype-not-req`). Strings of 256 characters in a round trip; 257 characters are rejected at encode and decode (#87). Round trip of all types, exact 64-bit integers, wire format, NDEATH without seq. SPB-16 properties. SPB-19 Bytes, File and DataSet. SPB-18 template definition and instance. The worst-case DBIRTH in the buffer |
| `test_node` | 57 | The Edge Node engine. SPB-3 (#161, `tck-id-topics-nbirth-bdseq-increment`): the bdSeq starts at 0 at each boot and increases by 1 for each CONNECT. #164: a failed NBIRTH of a Rebirth ends the session. #166 (`tck-id-payloads-name-birth-data-requirement`): each metric has a timestamp. SPB-10 (#163): the datatype of a DCMD. #158 and #76: a DBIRTH that does not fit. #160: no NDATA or DDATA before a valid clock. #172 device prefix. #173/#174 UMH preset. #132 `Node Info/Core Dump`. #144/#223 `Node Control/Boot Other Slot` only with `ota.mqtt`. FW-1 metrics and URLs (#87). SPB-16 to SPB-19, SPB-15 Scan Rate, SPB-17 diagnostics, TIME-3, IO-4 counters. Store & forward with `is_historical`. Births, deaths, aliases, the shared seq, rebirth, NCMD and DCMD, discards (#60), "session up" only after the NBIRTH (#71), no DDATA without a DBIRTH (#73) |
| `test_poll` | 27 | Polling against a simulated slave: offline after N cycles, exceptions, a block without a response (#66). Counters per device, FC06 write with read-back, FC15/FC16, NaN on coils, out of range. The probe with backoff of offline devices (MB-9, #61). MB-6 decode and writes, MB-12 fast and slow scan, MB-14 a write between requests, SPB-15. Modbus TCP counters (#69) |
| `test_netsel` | 7 | NET-4 interface selection: Ethernet only, Wi-Fi as backup or primary, a link that bounces, `failback_s` 0. #57: an interface without a path to the broker |
| `test_brokersel` | 6 | MQTT-6 broker list. #156 (`tck-id-operational-behavior-edge-node-termination-host-offline-reconnect`): a valid offline STATE of the primary host moves to the next broker at once |
| `test_phost` | 4 | SPB-13: valid STATE payloads, 18 invalid payloads, old timestamps ignored. #157 (`tck-id-operational-behavior-host-application-multi-server-timestamp`): one STATE timestamp for each MQTT server |
| `test_sf` | 24 | SPB-14 store & forward and the spill to the microSD card (#124, #136, #142, #143, #151, #152), with a simulated card. Order, overflow, recovery in bounded steps, checkpoints, a maximum of 64 KB of duplicates, configuration changes, a full card, lost superblocks, damaged blocks, I/O failures (#134). A fault test of 376 restarts (power cut, torn write, lost writes, a damaged write). The move of the RAM ring before a planned restart (#136). Ten mutations of the code each make at least one case fail |
| `test_sfcfg` | 5 | #145: the configuration hash of the spill. A golden value, 30 excluded fields that keep the hash, each field in the hash that changes it |
| `test_authlimit` | 3 | #64 login lockout. #110 (`test_webstall`): the stall check of the web server, and the wrap of the 32-bit clock |
| `test_syslog` | 5 | REL-4: severity, the line parser, RFC 3339 timestamps, the RFC 5424 format, a full ring without a wait, discards |
| `test_auth` | 6 | SHA-256, HMAC, PBKDF2, the password hash, Basic auth. #100 CSRF check. #106 canonical `Host` and `Origin`, and 17 malformed authorities |
| `test_cfgdoc` | 11 | Secrets in the document (mask, delete, migration), errors with a path, masked export and re-import, the Modbus keys after re-import. #187 the PMK of the access point. #91 the print is byte for byte `cJSON_Print`. #106 the JSON lock with two threads |
| `test_button` | 4 | BOOT: reset at 10 s, the 2–10 s gesture, a button pressed from boot, bounces, clock wrap |
| `test_mbap` | 7 | MB-15 Modbus TCP: MBAP frames, forwarded functions, exceptions 01/03/0A/0B, the client list, the refusal through the access point (#120) |
| `test_dout` | 12 | TCA9554: polarity of 8 channels, boot sequence, read-back, bus errors, retry with backoff (#74). IO-3: the latch of `hold` outputs |
| `test_localio` | 10 | IO-3 boot state and safe state, IO-4 save schedule, IO-7 rules `follow`, `invert`, `toggle` |
| `test_buzz` | 5 | IO-6 beeps, alarms, priority, a silent buzzer |
| `test_rtc` | 7 | TIME-2 PCF85063 registers ↔ UTC (each day 2025–2099), the OS bit, invalid fields. #160: the sources of the clock and `POST /api/time` |
| `test_ota` | 17 | FW-1 (#144): the expected mode, the self-check, the backstop of 5 min, the rejection of a configuration, the retry of a failed confirmation (#99). The capability block and the pre-flight check (#102, #148). URLs, the request body, the progress |
| `test_cfgschema` | 6 | FW-1 (#144 item 3): the schema that a stored configuration needs |
| `test_bootloop` | 6 | FW-1 (#144 item 4): the boot-loop counter and safe mode |
| `test_fwlast` | 7 | FW-1 (#144 items 5 and 7): the record of the last update |
| `test_lkgnet` | 5 | CFG-3 / FW-1 (#144 item 8): the network of the last valid configuration, readable by an earlier or a later image. A field without its terminator |
| `test_claim` | 9 | CFG-3 / NET-5 (#120): the claim window and the password rules |
| `test_wpa` | 8 | NET-5 (#187): SHA-1, HMAC-SHA1, PBKDF2-HMAC-SHA1, the WPA2 PMK vectors, the SSID |
| `test_aplife` | 10 | NET-5 (#187): the life of the access point, with a fake clock |
| `test_cdump` | 6 | REL-1 (#132): base64 of the core dump, the status and summary JSON |
| `test_mbdiag` | 5 | #182: `POST /api/modbus/read` |
| `test_nvsraw` | 21 | The raw NVS pages: plaintext, encrypted and mixed partitions (#67, #94, #107, #109), the migration copy, the boot decision, `may_erase`, the copy out of reach (#104, #108), FULL and FREEING pages (#97), a lost namespace entry (#185) |
| `test_cardfmt` | 7 | #137, #139: the format requests of the card, and their text and JSON |
| `test_cfgprev` | 8 | FW-1 (#146): the configuration from before an update, and the downgrade decision. #226: a repeated key in the comparison of two documents |
| `test_ident` | 4 | #207: the identify pattern of the LED and the buzzer |
| `test_logring` | 6 | #199, #200: the ring of the live log and the log levels |
| `test_mblocal` | 8 | #216: the local unit of the Modbus TCP server (DIs, DOs, counters) |
| `test_certs` | 9 | #205, #221: the rules of the certificate files |
| `test_wifiown` | 5 | #208: the owners of the Wi-Fi driver |

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
| `fuzz_spb` | MQTT | `spb_decode_with` with each callback (DataSet columns and cells, template members), `spb_rx_number` for each datatype, `spb_rx_prop`. A payload that the validation pass accepts must decode in full |
| `fuzz_node` | MQTT | Up to 8 NCMD, DCMD (also with a topic of the input) and STATE messages for a node in session (`gw_node_handle_message`, `gw_phost_on_state`), with the two configurations of the tests. Each payload that the node publishes must decode |
| `fuzz_config` | The web UI, the REST API, the configuration file | `gw_config_parse` and `gw_config_parse_schema` with an allocator that fails at a selected allocation; on a valid document `gw_config_lint`, the device IDs, the mDNS name, `gw_sfspill_cfg_hash` and the round trip of `gw_lkg_encode` and `gw_lkg_decode`. Also `gw_filerefs_parse` and `gw_json_depth` |
| `fuzz_cfgdoc` | The web UI, the REST API, the shell, the configuration file | `gw_cfgdoc_apply` in both modes, `gw_cfgdoc_export` (no stored secret in the output), `gw_jsonfeed_add` in pieces, `gw_cfgprev_diff` (a document has no difference from itself) |
| `fuzz_web_json` | The REST API, MQTT | `gw_claim_parse`, `gw_mbdiag_parse` and `gw_mbdiag_answer` (the answer must be JSON), `gw_iocmd_parse`, `gw_ota_parse_request` and `gw_ota_url_ok`, `gw_logring_parse_set`, `gw_phost_parse` |
| `fuzz_web_text` | HTTP headers, short fields, uploaded files | `gw_auth_parse_basic`, `gw_base64_decode`, `gw_auth_check_password`, `gw_auth_csrf_check`, `gw_ota_url_ok`, `gw_ota_sha256_parse`, `gw_san_has_host`, `gw_wpa_pass_check`, `gw_wpa_pmk_hex_valid`, `gw_pem_key_blocks`, `gw_config_file_name_ok`, `gw_filerefs_users` |
| `fuzz_flash` | The flash, the RTC chip, a firmware image | `gw_lkg_decode`, `gw_fwlast_parse`, `gw_cfgprev_parse` (each with its round trip), `gw_rtc_decode`, `gw_ota_caps_parse`, `gw_ota_caps_accept`, `gw_ota_preflight` |
| `fuzz_nvs` | The flash, LittleFS | The raw NVS pages (`gw_nvsraw_scan_page`, the u8 lookup, the erase marker) and the copy of a migration (`gw_nvsmig_load`, `gw_nvsmig_check`, `gw_nvsmig_next`, `gw_nvsmig_find`, `gw_nvsmig_orphan_in_copy`). The target can correct the CRCs of the input |
| `fuzz_text` | Many | `gw_utf8_valid` against `gw_utf8_seq`, `gw_parse_ipv6` against `inet_pton`, `gw_hostname_from`, `gw_config_file_name_ok`, `gw_logtag_valid`, `gw_loglevel_parse`. Log lines: `gw_syslog_parse_line`, `gw_syslog_format`, `gw_logring_put`, `gw_logring_copy`, `gw_logring_tag`, `gw_logtags_add` |
| `fuzz_sfspill` | The microSD card | The boot recovery of the spill (`gw_sf_step`, `gw_sfspill_prepare`) on a card image of the input (2 extents of 2 blocks), the replay to a node (`gw_node_flush_history`; each payload must decode), then new blocks on that card. The target can correct the CRCs of the superblocks and of the blocks |

- **Seed corpus.** The build runs `fuzz_seeds`. It writes valid inputs for each target into `<build>/seeds/<target>`. It makes them with the encoders of `gw_core` (Modbus frames, the births and commands of a node, the records, an NVS page, a migration copy, a card that the spill wrote), and from `spec_example.json`, `default_config.json` and `tools/e2e/fs/config.json`. Thus the seeds follow a change of a format.
- **Dictionaries.** `json.dict` has the JSON tokens and the keys and values that `gw_core` reads (for `fuzz_config`, `fuzz_cfgdoc` and `fuzz_web_json`). `spb.dict` has the metric names of the commands and the topics (for `fuzz_spb` and `fuzz_node`). Add a new key of the configuration to `json.dict`.
- **Global state.** `fuzz_config` and `fuzz_cfgdoc` set the allocator of the parser (`gw_config_set_allocator`) back to `malloc` and `free` after each input. The parser sets the cJSON hooks again at each call. `fuzz_node` starts each input from a copy of the node after its births, and `fuzz_sfspill` and `fuzz_node` reset `gw_tagdb`.
- **Inputs from outside the gateway.** A buffer with a length goes to the function in a heap block of exactly that length, without a NUL after it (for example a topic, as esp-mqtt gives it). AddressSanitizer then finds a read of one byte past the end.
- **Run.** `idf/test/fuzz/run_fuzz.sh <build> <seconds> [target ...]` runs the targets, one for each CPU at a time. It writes a table with the executions, the coverage and the corpus of each target. A finding goes to `<build>/crashes/` and stops the script with exit code 1.

### The findings

| Target | Input | Cause | Correction | Regression test |
|---|---|---|---|---|
| `fuzz_text` | `::ffff:40.1.062.1` | `parse_ipv4` in `gw_config.c` used `sscanf("%u")`. It accepted a leading zero (`062`), a sign (`+1`, `-0`) and a space before a number. `inet_pton` rejects them, and other programs read `062` as octal. The prefix of `modbus_tcp.allowed_clients` had the same problem: `/-0` gave the prefix 0, which permits each client | Decimal numbers without a leading zero, a sign or a space, in the IPv4 addresses and in the prefix | `test_config`: `test_ipv6_literals`, `test_wifi_section` and `test_modbus_tcp_section` |
| `fuzz_cfgdoc` | `{"":1,"":""}` | `gw_cfgprev_diff` found a repeated key of an object only at its first item. A document with a repeated key had a difference from itself | The items of two objects pair by key and by the occurrence of that key | `test_cfgprev`: `test_diff_repeated_key` |

### Results of the fuzz tests

The local runs were on a cloud container with 4 CPUs, with clang 18.1.3, 4 targets at a time (runs of 20 s, 60 s, 300 s, and 600 s or 900 s for the targets with the most code). The time and the executions are the sums of these runs. The coverage and the corpus are those at the end of the last run. The two findings above came in the first runs of 20 s. After their corrections, no target found an error.

| Target | Time (s) | Executions | Coverage (edges) | Corpus (inputs) |
|---|---|---|---|---|
| `fuzz_mb_rtu` | 380 | 150 million | 165 | 205 |
| `fuzz_mbap` | 980 | 107 million | 295 | 1500 |
| `fuzz_spb` | 1280 | 22 million | 366 | 2166 |
| `fuzz_node` | 1300 | 2.8 million | 833 | 2618 |
| `fuzz_config` | 1280 | 4.1 million | 958 | 5775 |
| `fuzz_cfgdoc` | 1280 | 2.5 million | 995 | 5189 |
| `fuzz_web_json` | 1040 | 96 million | 626 | 5388 |
| `fuzz_web_text` | 380 | 106 million | 428 | 2792 |
| `fuzz_flash` | 380 | 60 million | 263 | 7678 |
| `fuzz_nvs` | 380 | 6.8 million | 168 | 632 |
| `fuzz_text` | 980 | 117 million | 349 | 1278 |
| `fuzz_sfspill` | 1010 | 0.7 million | 779 | 1430 |

`fuzz_node` and `fuzz_sfspill` are the slowest targets (approximately 1 600 and 700 executions each second), because each input encodes and decodes Sparkplug payloads. The other targets do 2 000 (`fuzz_cfgdoc`) to 390 000 (`fuzz_mb_rtu`) executions each second.

### Known unstable test

`test_poll` (`test_unanswered_block_fails_the_cycle`) failed one time in 7 runs with `Expected …756 Was …757`. The timestamp of a tag that the firmware did not read changed by 1 ms. It seems to depend on the wall clock of the host.

## Sparkplug decode check

`spb_dump` writes real NDEATH, NBIRTH, DBIRTH, DDATA and NDATA payloads of the engine. Each file name is `<n>_<group>+<TYPE>+<edge>[+<device>].bin`: the topic after `spBv1.0/`, with `+` for `/` (#166). `spb_decode.py` compiles the official `sparkplug_b.proto` of eclipse/tahu (an unchanged copy in `idf/components/gw_core/proto/`) with `grpc_tools.protoc`, and decodes the payloads with Google protobuf.

The payloads contain these items: UInt64, Int64, Double, String and register bits; the `Counter/n` counters of IO-4; the SPB-16 properties; `is_null` with BAD quality; a `bytes` tag; DataSets; a File; templates. `--check` applies these rules:

- The structure of the DataSets and of the templates (a definition only in the NBIRTH, without alias).
- `bdSeq` is Int64 in the NDEATH and the NBIRTH (#154).
- `Node Control/Rebirth` has no alias (#153, `tck-id-operational-behavior-data-commands-rebirth-name-aliases`).
- NDATA and DDATA metrics have no datatype (#159, `tck-id-payloads-metric-datatype-not-req`), and each value is in the field of the datatype of its birth.
- Each metric has a timestamp, also each member of a template instance (#166, `tck-id-payloads-name-birth-data-requirement`).
- The seq increases by 1 after the NBIRTH, 255 followed by 0 (#166, `tck-id-payloads-sequence-num-incrementing`).
- Each alias is unique across the NBIRTH and the DBIRTHs of the node (#166, `tck-id-payloads-alias-uniqueness`).

`idf/tools/test_spb_decode.py` is the self-test of these rules: each rule rejects a payload that breaks it. The E2E also decodes each message that it captures with the same rules (check "6. payloads decode").

## Firmware build

`idf.py set-target esp32s3 build` gives 0 warnings. The QEMU builds (`build-qemu`, `build-qemu-psram`, `build-qemu-plain` and `build-qemu-poison`) also give 0 warnings. The components of the project compile with the options of [Compiler warnings and the stack](compiler-warnings-and-stack.md) (#227).

During the configuration, ESP-IDF v6.1 prints 5 CMake warnings of its own (`component_validation.cmake`: private directories of `esp_wifi` that `wpa_supplicant` uses). They do not come from the project or from the compiler.

## Linters

The `host-tests` job runs the linters (#229). Each linter uses only rules that find errors, and no style rules. A result stops the job.

| Linter | Scope | Configuration |
|---|---|---|
| ruff | The Python files in `idf/tools/` | `idf/tools/ruff.toml`: `F`, `E9`, `PLE`, `B` |
| shellcheck | Each `*.sh` file of the repository | — |
| actionlint | `.github/workflows`, with shellcheck on each `run:` script | — |
| zizmor | The security of the workflows, with `--offline` | Each action is pinned to a full commit hash. `actions/checkout` has `persist-credentials: false` |
| ESLint | The inline `<script>` of `index.html` and `claim.html` | `idf/tools/lint/eslint.config.mjs`: `eslint:recommended` with the type "problem", `no-redeclare`, `no-global-assign`. Needs Node.js 20.19 or later |

- The versions are pinned. ruff is in `idf/tools/e2e/requirements.txt`. shellcheck, actionlint and zizmor are in `idf/tools/lint/requirements.txt`, as Python wheels with the binary. ESLint is in `idf/tools/lint/package-lock.json`.
- Each `zip()` has a `strict=` argument (B905). Use `strict=False` only for an intended truncation.
- A false positive gets `# noqa: <rule>` or `# shellcheck disable=SCnnnn`, with the reason.
- Do not add an ESLint comment to a web page, because the comment goes into the firmware image. For a false positive, change an option in `eslint.config.mjs`.

## CI

`.github/workflows/ci.yml` runs on each push and pull request, in the `espressif/idf:v6.1` image, pinned by digest.

| Job | What it does |
|---|---|
| `host-tests` | Builds the host tests with `-O2` and runs them. Decodes the `spb_dump` payloads (`spb_decode.py --check`). Runs `test_spb_decode.py`, `test_harness.py` and the linters |
| `host-tests-sanitizers` | Builds the host tests with `-DGW_SANITIZE=ON` and runs them. Decodes the payloads of the sanitized `spb_dump` |
| `fuzz` | Installs `clang-18` and `libclang-rt-18-dev`, builds `idf/test/fuzz` and runs each target for 60 s, 4 at a time (`run_fuzz.sh`). The start is the corpus of the weekly job (from `actions/cache`, read only) and the seeds. A finding stops the job, and the artifact `fuzz-findings` has the input and the logs. The job takes approximately 5 min, at the same time as the other jobs |
| `firmware` | Runs `idf.py set-target esp32s3 build`. Fails on a `warning:`. Publishes the binaries, the `idf.py size` output and `stack.txt` |
| `e2e-plan` | Reads `CI_GROUPS` from `idf/tools/e2e/scenarios.py`, and gives the matrix of the two next jobs |
| `e2e-build` | One job for each variant (`default`, `psram`). Runs `run_e2e.sh` with `E2E_STAGE`, and publishes the images. The `psram` job also builds `build-qemu-plain`, because the group `nvs-main` of that variant uses it. Each job makes its own throwaway signing key, and the plain image accepts only an update that the same key signed. GitHub keeps the images for 1 day, because they contain the signing key of the run |
| `e2e` | One job for each group of `CI_GROUPS` (5 PSRAM groups and 1 default group). Runs `run_e2e.sh --only <group>` with `E2E_PREBUILT`. Publishes the logs, the payloads, `results.json`, `qemu-exits.txt` and the `fault-*.txt` dumps |
| `e2e-summary` | Writes one table of all the groups. Fails when a group has no `results.json` or a failed check, ran other scenarios than its `--only` list, or when a scenario of a full run ran in no group or in two groups. Also fails when the groups of one variant repeated more scenarios than `MAX_RETRIES` (2) |
| `e2e-poison` | Two jobs: `E2E_POISON=1 E2E_PLAIN=0 run_e2e.sh` with `--only ota-fw1` in the PSRAM variant, and with `--only ota-park,ota-trial` without PSRAM, on the build with full heap poisoning (#104). Refer to [Heap poisoning](e2e-harness.md#heap-poisoning) |

`.github/workflows/fuzz-weekly.yml` runs one time each week (Monday 02:17 UTC) and on request (#226). It runs each fuzz target for 20 min, 4 at a time (approximately 60 min), from the corpus of the previous week. Then it keeps only the inputs that add coverage (`FUZZ_MERGE=1`, `-merge=1` of libFuzzer). It saves the corpus in `actions/cache` (also after a finding) for the next week and for the `fuzz` job, and publishes it as the artifact `fuzz-corpus` (kept 90 days). A finding stops the job, and the artifact `fuzz-weekly-findings` has the input and the logs.

The GitHub containers do not permit `unshare`. Thus each `e2e` job runs on the loopback interface of its container.

The workflow pins each action to a commit hash, with the tag in a comment (#229). All the actions run on Node 24 (#235):

| Action | Version |
|---|---|
| `actions/checkout` | v7.0.1 |
| `actions/cache` | v6.1.0 |
| `actions/upload-artifact` | v7.0.2 |
| `actions/download-artifact` | v8.0.2 |
| `actions/setup-node` | v6.5.0 |

### The E2E groups

Each group has its own job, and the groups run at the same time (#121, #135, #233). The groups of a variant are exactly one full run of that variant.

| Variant | Group | `--only` |
|---|---|---|
| PSRAM | `ota-fw1-web` | `ota-fw1-web,nopsram` |
| PSRAM | `ota-fw1-ncmd` | `ota-fw1-ncmd,reset,slow` |
| PSRAM | `nvs-main` | `nvs,main` |
| PSRAM | `sf-tls` | `sf,tls,mqtt5,network,failover` |
| PSRAM | `other` | `failsafe,web,phost,https,time` |
| Default | `ota-trial` | `ota-park,ota-wiring,ota-trial` |

- The longest group sets the duration of the E2E jobs. `FW-1 OTA by web upload` is the longest scenario: it has two web uploads of 1 MB, each approximately 220 to 250 s with PSRAM. The durations are in [E2E durations](e2e-durations.md).
- The check "6. payloads decode" of each run needs at least one payload. `nopsram` and the #97 integration have no MQTT observer, so each is in a group with a scenario that has one.
- The harness makes 5 checks one time per run, after the scenarios: "6. payloads decode", #105, #92, #111 and SPB-5. Thus each group adds 5 checks. The total line of `e2e-summary` also gives the count as in one full run.
- `test_harness.py` (`CiGroups`) checks that the groups of each variant are one full run, that each scenario is in one group, that no group is empty, that each group has a scenario with an observer, and that the group of `GW42` gets the build without NVS encryption.
- If the `psram` jobs are too long, you can set `E2E_TCG_THREAD=multi` in the `e2e` job. Then the OTA scenarios are faster, but QEMU deaths occur, and the harness retries them.

### The caches of the CI

- The jobs `firmware`, `e2e-build` and `e2e-poison` use ccache (`IDF_CCACHE_ENABLE=1`). `actions/cache` keeps its directory, with one key for each job and variant.
- `CCACHE_COMPILERCHECK=content` is necessary. Each configuration writes a new `-specs` file into the build directory, so the default check (`mtime`) finds no entry.
- ccache gives the compiler messages again at a hit. Thus the check for 0 warnings of the `firmware` job stays valid.
- The 76 C files of the own components (`gw_*`, `main`) compile without ccache (#227). At a hit, ccache 4.9 does not give the `.ci` file of `-fcallgraph-info` again.
- The `e2e` and `e2e-poison` jobs keep QEMU and the venv of the harness in `actions/cache`. The keys contain the digest of the image, and the key of the venv also contains the hash of `requirements.txt`.
