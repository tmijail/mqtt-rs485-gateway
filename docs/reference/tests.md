# Tests

This document gives the test suites of the firmware, what each suite examines, the pass criteria, the CI jobs and the current results. The E2E tests in QEMU have two documents of their own: [The E2E test bench](e2e-harness.md) (the emulator, the brokers, the variants, the retry rules and the limits of QEMU) and [The E2E scenarios](e2e-scenarios.md) (what each scenario asserts). To run the tests, refer to [Run the tests](../how-to/run-the-tests.md).

> **Warning: The firmware has never run on the board (#1, #18).** All the results in this document come from host tests and from QEMU. The W5500, the RS485 hardware mode, the TCA9554, the GPIO DIs and the LED are compile-only.

## The test levels

| Test | What it examines | Where it runs | Pass criterion |
|---|---|---|---|
| Host tests (QA-1) | The logic of `gw_core` in pure C, without ESP-IDF: 42 Unity suites, and checks of the `sdkconfig`, the Kconfig names and the web form | `idf/test/host`, CTest | Each CTest test passes. 0 warnings with `-Werror` |
| Host tests at `-O2` | The same tests, with the warnings that only the optimizer gives | CI job `host-tests` | The same |
| Host tests with sanitizers (#225) | The same tests with AddressSanitizer and UndefinedBehaviorSanitizer | CI job `host-tests-sanitizers` | No error from a sanitizer |
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
| CTest tests | 59: 42 Unity suites, `test_sfcfg_packed` and 16 checks |
| Unity cases | 448 in the 42 suites (453 with `test_sfcfg_packed`) |
| Self-test of the decode rules | 26 tests |
| E2E checks, full run of the PSRAM variant | 264 (284 in the 5 CI groups: each group repeats the 5 checks of the run) |
| E2E checks, full run of the default variant | 18 |
| Optional E2E scenarios | `umh` 15 checks, `tck` 19 checks, `downgrade` 7 checks |

The last full run was on `main` 6288614 (stage 1 of #224), on a cloud container with 4 CPUs (`run_ci_groups.sh`, `PARALLEL=2`, `E2E_TIMEOUT_SCALE=1.5`). The PSRAM variant gave 284/284 checks in the groups, and the default variant gave 18/18. There were no retries and no QEMU exits. The #80 check gave a minimum free internal RAM of 155 192 B and a largest block of 118 784 B at a validation. The host tests gave 59/59 at `-O0`, at `-O2` and with the sanitizers, and the linters gave 0 results.

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
| 11 `sdkconfig_check_*` tests (#83, #93, #103, #135) | `cmake/gw_sdkconfig_check.cmake` stops the build when a required option drifts. A stale `sdkconfig` fails, and the `warn` mode only warns. An `sdkconfig` without the PSRAM fails, except with the QEMU overlay |

The `sdkconfig` check (#83) covers only the options in `GW_SDKCONFIG_REQUIRED`. If the firmware starts to depend on a new option, add it to that list. An intentional change of one of these options in `menuconfig` also stops the build: use `-D GW_SDKCONFIG_CHECK=warn`.

### The Unity suites

| Suite | Cases | Scope |
|---|---|---|
| `test_mb_rtu` | 6 | CRC (vectors of the Modbus specification), FC01–06/15/16 frames, response length, exceptions, incorrect echo |
| `test_mb_master` | 10 | t3.5 and the delay between frames, timeout and retries, bad CRC, incorrect unit, exception without a retry. The wait for the end of transmission is proportional to the frame (FC16 of 13 B at 1200 baud with parity, and the longest FC16). #112 (`test_attempt_bound`): the longest attempt of the slowest valid configuration stays below the 15 s TWDT. A request with a quantity of 0 is refused |
| `test_conv` | 13 | MB-6: a scaled uint32 above 2^24 keeps each digit (#165). The `bytes` tag (BADC/DCBA, hexadecimal). The datatype map, also Int64/UInt64/Double/String, and 64-bit scaled values to Double. The orders ABCD/CDAB/BADC/DCBA in 32 and 64 bits, exact 2^53 + 1, INT64_MIN, float64. Strings (fill characters, NUL, non-printable characters, BADC, maximum length). Inverse scale (12.3 → 123), ranges |
| `test_logic` | 8 | RBE and deadband against the last reported value (drift), exact 64-bit RBE and string RBE (hash), validity and NaN, debounce, backoff 1–60 s, offline |
| `test_config` | 47 | CFG-1 (#162): well-formed UTF-8 in the Sparkplug IDs and names, no two names that differ only in case (`tck-id-case-sensitivity-sparkplug-ids`, `tck-id-case-sensitivity-metric-names`), no metric that is also a folder of another metric. #172: `sparkplug.device_prefix` and the mDNS instance name. #169: the name rules for UNS hosts give warnings with JSON paths, and the UMH preset makes them errors. Each section and its limits: `ota`, `eng_low`/`eng_high`, the `bytes` tag, the device `type`, `sparkplug.diagnostics_s`, `mqtt.tls`, the broker list, `mqtt.protocol`, `sparkplug.store_forward` (#31), `web`, `local_io` (IO-3, IO-4, IO-6, IO-7), `network.hostname`, `mdns`, `syslog`, `network.wifi`, `network.ipv6` (16 invalid IPv6 forms), MB-6, `block_reads`/`max_gap`, `scan`/`slow_poll_ms`, `modbus_tcp`. Errors with a path, line and column, duplicate keys, the maximum nesting, the limits 16/64/256, the maximum document (256 tags, > 40 KB), no available memory. An unknown key longer than the path buffer ends its path with `...` |
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
| `test_cfgprev` | 7 | FW-1 (#146): the configuration from before an update, and the downgrade decision |
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
| `firmware` | Runs `idf.py set-target esp32s3 build`. Fails on a `warning:`. Publishes the binaries, the `idf.py size` output and `stack.txt` |
| `e2e-plan` | Reads `CI_GROUPS` from `idf/tools/e2e/scenarios.py`, and gives the matrix of the two next jobs |
| `e2e-build` | One job for each variant (`default`, `psram`). Runs `run_e2e.sh` with `E2E_STAGE`, and publishes the images. The `psram` job also builds `build-qemu-plain`, because the group `nvs-main` of that variant uses it. Each job makes its own throwaway signing key, and the plain image accepts only an update that the same key signed. GitHub keeps the images for 1 day, because they contain the signing key of the run |
| `e2e` | One job for each group of `CI_GROUPS` (5 PSRAM groups and 1 default group). Runs `run_e2e.sh --only <group>` with `E2E_PREBUILT`. Publishes the logs, the payloads, `results.json`, `qemu-exits.txt` and the `fault-*.txt` dumps |
| `e2e-summary` | Writes one table of all the groups. Fails when a group has no `results.json` or a failed check, ran other scenarios than its `--only` list, or when a scenario of a full run ran in no group or in two groups. Also fails when the groups of one variant repeated more scenarios than `MAX_RETRIES` (2) |
| `e2e-poison` | Runs `E2E_POISON=1 E2E_PLAIN=0 run_e2e.sh --only ota-fw1,ota-park,ota-trial` on the build with full heap poisoning (#104). Refer to [Heap poisoning](e2e-harness.md#heap-poisoning) |

The GitHub containers do not permit `unshare`. Thus each `e2e` job runs on the loopback interface of its container.

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
