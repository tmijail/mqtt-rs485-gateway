# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware for an RS485 (Modbus RTU) → MQTT Sparkplug B 3.0 gateway on the Waveshare ESP32-S3-POE-ETH-8DI-8DO board, built on ESP-IDF v6.1 (target `esp32s3`). Everything lives in `idf/`. `docs/SPEC.md` is the product spec; requirement IDs such as MB-6, SPB-14 and FW-1 come from it, and code, tests and commits refer to them and to GitHub issue numbers (`#80`, `#107`, …).

**This is an open-source project, not a product to flash, lock and sell.** The firmware runs on a board that anyone can buy, and the owner of the board must always be able to reflash it, read it and change it. Do not design for a vendor that controls the devices of its users:
- The default build must never burn eFuses or take away the owner's access: no Secure Boot, no flash encryption in release mode, and no disabled download mode.
- The eFuse-burning features are not in `main` (decision of #133): Secure Boot V2 (FW-2), flash encryption (FW-3), NVS encryption with an HMAC key in eFuse on the board (the eFuse part of CFG-7), and the disabled download mode. The overlay `sdkconfig.production`, `partitions-production.csv`, the signing scripts and their CI job are on the branch `archive/efuse-production`, as they were in `main` at commit 4a0e01c. That branch gets no maintenance. Do not bring them back to `main`, and do not add other features that burn eFuses (for example the DS peripheral for the MQTT client key).
- Two items of #133 still need a decision of the product owner: the signed OTA images without Secure Boot (`SECURE_SIGNED_APPS_NO_SECURE_BOOT`) and the NVS encryption with its migration code. Both are only in the QEMU build. Do not add new work that depends on them until #133 decides.
- When a requirement in `docs/SPEC.md` assumes a vendor product (factory stations, locked devices), question it before you implement it.

`docs/` is the authoritative, detailed reference (index: `docs/README.md`). Read the relevant document before changing a subsystem:
- `docs/reference/architecture.md` and `docs/explanation/architecture.md`: the components, the task table, the concurrency rules and the memory budget.
- `docs/how-to/build-the-firmware.md` and `docs/reference/build-configuration.md`: the build, the overlays and the partition table.
- `docs/reference/configuration.md`, `docs/reference/commissioning-and-secrets.md` and `docs/reference/firmware-updates.md`.
- `docs/reference/tests.md`, `docs/reference/e2e-harness.md` and `docs/reference/e2e-scenarios.md`: the tests, the test environment and the latest results.
- `docs/reference/sparkplug-behavior.md`, `docs/reference/known-limitations.md` and `docs/reference/requirements-status.md`.

The root `README.md` is the short project page: what the gateway does, the status of the project, the features and the links. Do not put measurements, test results or per-issue details in it; put them in `docs/`. `idf/README.md` is only a short guide to the folder `idf/`. Some old docs and GitHub issues are in Spanish; all new writing follows **Writing standard** below.

## Writing standard: ASD-STE100 Simplified Technical English

Write all new or changed prose in ASD-STE100 Simplified Technical English (STE). This includes READMEs, `docs/`, GitHub issues, issue and PR comments, PR descriptions and commit messages. Code comments and identifiers stay in normal technical English.

- **Vocabulary:** Use the STE dictionary words only in their approved meaning and part of speech: one word, one meaning. For example, "test" (verb) means "examine for correct function", "show" means "let someone see". Use technical names (component, Kconfig option, API, signal) and technical verbs (build, flash, publish, poll, encrypt) as they are. Keep one term for one thing in the full document.
- **Sentences:**
  - Procedures (steps): max 20 words.
  - Descriptions: max 25 words.
  - One topic per sentence; one instruction per sentence, unless the actions happen at the same time.
  - Paragraphs: max 6 sentences.
- **Grammar:**
  - Procedures use the imperative ("Delete the `sdkconfig`. Do the reconfiguration again.").
  - Descriptions use the active voice and the simple tenses: present, past and future.
  - Do not use the -ing form as a noun or as a modifier. Do not use phrasal verbs.
  - Use articles ("the", "a"), and avoid long noun clusters: write "the margin of the internal RAM", not "the internal RAM margin value".
- **Warnings and cautions:** Start them with a clear instruction (the command). Then give the risk.
- **Existing Spanish text:** When you change a section, rewrite that section in STE. Translate full files only when the user asks.
- **Structure (Diátaxis):** Put each new document in `docs/` into one Diátaxis type: `docs/tutorials/`, `docs/how-to/`, `docs/reference/` or `docs/explanation/`. Do not mix the types in one document. Link to the other type instead (for example, from a procedure to the reasons for it).

**The firmware has never run on real hardware.** It has only run in host tests and in QEMU. The W5500, RS485 hardware mode, TCA9554, GPIO DIs and LED are compile-only (issues #1 and #18).

**No gateway is installed yet (before v1.0).** Until the product owner says that the first gateway is installed:
- A change of a stored format (the configuration document, NVS keys and records, the microSD card format, RTC memory) needs no migration and no backward compatibility. Document the change in the reference document of that format in `docs/reference/`, and say in the commit that old data is dropped or reset.
- Do not add code that reads an earlier format only for compatibility.

When the product owner says that the first gateway is installed, tag that commit `v1.0`. From then on, each change of a stored format needs an upgrade path and a downgrade path (see #144, #146, #147 and #148).

## Sparkplug B conformance

The gateway must obey the Eclipse Sparkplug B 3.0.0 specification fully. Do not accept a deviation from it. The only exception is a compatibility profile for one host (see the last item).
- Obey each MUST, MUST NOT, SHOULD and SHOULD NOT statement for an Edge Node. A deviation is a bug, also when a document records it.
- The reference is the normative text with a `tck-id`, in the AsciiDoc chapters at the tag `v3.0.0` of `eclipse-sparkplug/sparkplug`. When an example does not agree with the normative text, obey the normative text.
- When `docs/SPEC.md` does not agree with the Sparkplug specification, the Sparkplug specification is correct. Correct `docs/SPEC.md`.
- When the specification permits a choice (MAY), write the choice and the reason in `docs/reference/sparkplug-behavior.md`.
- When a change concerns a Sparkplug rule, give its `tck-id` in the code comment, the test, the issue and the commit.
- When a host does not obey the specification (for example, the Sparkplug 2.x STATE of UMH Core), write the limit in the documentation and report the problem to the host project. Do not change the default behaviour of the gateway for it.
- An option for a specific host must obey the specification, if possible. A compatibility profile that does not obey it is a last resort. Add one only when all these conditions are true:
  - A test with the real host shows that the profile is necessary.
  - The problem was reported to the host project first, and the host project did not correct it.
  - The product owner approves the profile.

  The profile is off by default and changes only the necessary items. `docs/reference/sparkplug-behavior.md` lists each deviation that it causes.

## The web UI configures everything

Each key of the configuration document must have a control in the Configuration tab of the web UI. The JSON tab alone is not sufficient. This is a principle of the project (decision of the product owner, #221).
- A change that adds or changes a configuration key also adds or changes its control, in the same change. The control shows the validation error at its field (#191). A control that does not explain itself gets an explainer (#190).
- A file that a key refers to (for example a PEM file in LittleFS) must be possible to load from the web UI.
- A large change of the web UI starts with a prototype in the design canvas (a Claude Design artifact, private to the product owner): https://claude.ai/artifact/FcFdKXktaEWXxS5bSnDNc7. A large change adds a tab, a card or a dialog, or changes a layout. Start its implementation only after the product owner approves the prototype in a comment on the issue. These issues have the label `design first`.
- A small change of the web UI needs no prototype (decision of the product owner, 2026-11-18). A small change adjusts an existing control: its validation, the normalization of its input, its text or its explainer. The product owner can ask for a prototype for any change.
- Some existing keys have no control yet (#221 lists them). Do not use this as a reason to add a new key without a control.

## Commands

The host tests and the decode check run from the repo root. The firmware commands are run from `idf/`, after `. $IDF_PATH/export.sh`.

```bash
# Host tests (pure C, no IDF): 42 Unity suites + sdkconfig, guard and rule checks (#228), -Werror.
# The rule checks need GCC 10 or later, nm and Python 3; with another compiler, add -DGW_RULE_CHECKS=OFF
cmake -S idf/test/host -B build-host && cmake --build build-host -j && ctest --test-dir build-host --output-on-failure
ctest --test-dir build-host -R test_poll      # one suite; or run build-host/test_poll directly
# The host tests at -O2, as the CI job host-tests builds them: some warnings show only with the optimizer
cmake -S idf/test/host -B build-o2 -DCMAKE_C_FLAGS=-O2 && cmake --build build-o2 -j && ctest --test-dir build-o2 --output-on-failure
# The same host tests with AddressSanitizer and UndefinedBehaviorSanitizer (#225; CI job host-tests-sanitizers)
cmake -S idf/test/host -B build-asan -DGW_SANITIZE=ON && cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure

# Fuzz tests: 12 libFuzzer targets for the parsers of gw_core, with ASan and UBSan (#226; CI job fuzz
# 60 s each, fuzz-weekly.yml 20 min each). Needs clang with libFuzzer (Ubuntu: clang-18 libclang-rt-18-dev)
cmake -S idf/test/fuzz -B build-fuzz -DCMAKE_C_COMPILER=clang && cmake --build build-fuzz -j
idf/test/fuzz/run_fuzz.sh build-fuzz 60                 # all targets, $(nproc) at a time (JOBS=n); a table per target
idf/test/fuzz/run_fuzz.sh build-fuzz 300 fuzz_spb       # one target; a finding goes to build-fuzz/crashes/
build-fuzz/fuzz_spb build-fuzz/crashes/<file>           # one input again; fix it in gw_core, add it to a Unity suite

# Sparkplug interop: decode generated payloads with the official Tahu proto
mkdir -p /tmp/spb && build-host/spb_dump /tmp/spb && python3 idf/tools/spb_decode.py --check /tmp/spb

# Firmware build (CI requires 0 warnings)
idf.py set-target esp32s3 build && idf.py size
# GCC static analyzer (-fanalyzer) on the own components gw_* and main only, in its own build dir
# (#230; CI job analyzer). With -Werror a result stops the build; the release build is unchanged
idf.py -B build-analyzer -D GW_ANALYZER=ON set-target esp32s3 build

# E2E: real firmware in QEMU against mosquitto + a pymodbus RTU slave. Each variant has its own
# full run (#135, scenarios.py DEFAULT_VARIANT_SCENARIOS); the two together are the full E2E.
python3 -m venv ~/e2e && ~/e2e/bin/pip install -r tools/e2e/requirements.txt
E2E_PSRAM=1 E2E_PYTHON=~/e2e/bin/python tools/e2e/run_e2e.sh     # PSRAM variant: the board's configuration, all but 3 scenarios (~22 min)
E2E_PYTHON=~/e2e/bin/python tools/e2e/run_e2e.sh                 # variant without PSRAM: only the OTA trial scenarios (~4 min)
E2E_PYTHON=~/e2e/bin/python tools/e2e/run_e2e.sh --only ota,https   # subset of scenarios (--only selects in any variant)
#  --only: failsafe,network,web,reset,tls,failover,mqtt5,phost,sf,https,time,ota,ota-fw1,ota-fw1-ncmd,
#          ota-fw1-web,ota-park,ota-wiring,ota-trial,nvs,slow,nopsram,refuse,main,umh,tck,downgrade
#          (ota = ota-fw1,ota-park,ota-wiring,ota-trial; ota-fw1 = ota-fw1-ncmd,ota-fw1-web, #233; an
#          unknown name is an error; nopsram needs E2E_PSRAM=1)
#  Optional scenarios (never in a full run or in the CI groups; they SKIP when the tool is absent):
#    umh  real benthos-umh v0.16.2 (UMH Core), E2E_BENTHOS_UMH=<binary>        (#175, docs/reference/e2e-scenarios.md)
#    tck  Eclipse Sparkplug TCK 3.0.0 on HiveMQ CE, E2E_TCK_DIR=<dir from tools/e2e/tck_setup.sh>
#         (#176, docs/how-to/run-the-sparkplug-tck.md)
#    downgrade  the image of an earlier release on the flash that this image wrote,
#               E2E_DOWNGRADE_REF=<git ref> (built once into build-downgrade/; #148, docs/reference/e2e-scenarios.md)
E2E_PSRAM=1 E2E_PYTHON=~/e2e/bin/python tools/e2e/run_ci_groups.sh /tmp/gw-groups   # the CI groups of a variant: build once, run in parallel (run it for each variant)
#  Windows / no local IDF: the same in Docker (idf/tools/e2e/Dockerfile), see docs/how-to/run-e2e-in-docker.md
#  E2E_STAGE=<dir> (build the images into <dir>, run nothing), E2E_PREBUILT=<dir> (run on them, no build),
#  PARALLEL=n (groups at the same time); the groups are CI_GROUPS in tools/e2e/scenarios.py
#  E2E_POISON=1 (heap poisoning: the CI runs ota-fw1 with E2E_PSRAM=1, and ota-park,ota-trial), E2E_PLAIN=0 (skip the no-NVS-encryption build),
#  E2E_TIMEOUT_SCALE / --timeout-scale (slow host), --web-stress N, JOBS=n (build parallelism)

# Harness self-test and the linters (all run in the CI job host-tests; each linter must give 0 results, #229)
python3 idf/tools/e2e/test_harness.py
#  Pinned versions: ruff in tools/e2e/requirements.txt; shellcheck, actionlint and zizmor in
#  idf/tools/lint/requirements.txt; ESLint in idf/tools/lint/package-lock.json. Only rules that find errors.
~/e2e/bin/pip install -r idf/tools/lint/requirements.txt
~/e2e/bin/ruff check idf/tools                          # Python tools: F, E9, PLE, B (idf/tools/ruff.toml)
git ls-files -z '*.sh' | xargs -0 ~/e2e/bin/shellcheck  # all shell scripts
~/e2e/bin/actionlint -shellcheck ~/e2e/bin/shellcheck   # .github/workflows (and shellcheck on each run: script)
~/e2e/bin/zizmor --offline .github/workflows            # workflow security (actions pinned to a commit hash)
npm ci --prefix idf/tools/lint && idf/tools/lint/node_modules/.bin/eslint --config idf/tools/lint/eslint.config.mjs idf/components/gw_web/www/
node --test idf/test/web/test_ip4norm.mjs             # the IPv4 normalization of the web UI (#226; CI job host-tests)
#  A false positive gets "# noqa: <rule>" or "# shellcheck disable=SCnnnn" with the reason. The web pages
#  are in the firmware image: for them, change an option in eslint.config.mjs, not the page.
```

CI (`.github/workflows/ci.yml`) runs host-tests, fuzz, firmware, analyzer (#230), the E2E jobs and e2e-poison on every push, in the pinned `espressif/idf:v6.1` image (fuzz and the host tests run directly on `ubuntu-24.04`). `.github/workflows/fuzz-weekly.yml` runs the fuzz targets longer each week and keeps their corpus (#226). The E2E jobs build the QEMU images one time per variant (`e2e-build`). Then they run the groups of `scenarios.py` in parallel jobs (`e2e`), and `e2e-summary` makes sure that the groups of each variant are one full run of that variant (#121, #135). A change is done when the host tests pass and the full E2E passes: the full run of the `E2E_PSRAM=1` variant and the full run of the default variant. Its results go in `results.json`, `qemu-exits.txt` and the `retries` list.

### sdkconfig pitfalls

- `sdkconfig.defaults` and the overlays only apply when an `sdkconfig` is **created**. After changing them, delete the build dir's `sdkconfig` and reconfigure. For example: `rm build-qemu/sdkconfig && idf.py -B build-qemu -D SDKCONFIG=build-qemu/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.qemu" reconfigure`.
- A stale `sdkconfig` shows up as undeclared `CONFIG_GW_*` symbols.
- `cmake/gw_sdkconfig_check.cmake` fails the build when required options drift. If the firmware starts depending on a new option, add it to `GW_SDKCONFIG_REQUIRED`.
- Overlays:
  - `sdkconfig.qemu` (the QEMU build, with `CONFIG_GW_IO_SIMULATED`);
  - `sdkconfig.qemu-psram`, `sdkconfig.qemu-plain` (no NVS encryption) and `sdkconfig.qemu-poison`, each stacked on `sdkconfig.qemu`.
- There is no production overlay in `main`. The eFuse-burning overlay `sdkconfig.production` is on the branch `archive/efuse-production` (#133).

## Architecture

### `gw_core`: all testable logic, pure C with no IDF

It holds:
- the Modbus RTU framing and master;
- polling and block planning;
- type conversion and RBE;
- the Sparkplug codec (`spb.c`, nanopb) and the Edge Node engine (`gw_node.c`: births, deaths, seq, aliases, NCMD/DCMD, rebirth);
- config parsing and validation (`gw_config`, `gw_cfgdoc`);
- broker and interface selection;
- store & forward;
- the OTA trial/rollback policy (`gw_ota`);
- raw NVS page parsing (`gw_nvsraw`);
- auth and CSRF helpers.

Platform calls go through `gw_port.h`, which has two implementations: `port/gw_port_esp.c` for the firmware and `port/gw_port_posix.c` for host tests. New logic belongs here, with a host test in `idf/test/host/`. A new parser of data from outside (the bus, the network, MQTT, the web, the flash, a card) also gets a fuzz target in `idf/test/fuzz/` (#226). The other components are thin IDF adapters around it.

### IDF components (`idf/components/`)

- `gw_bus`: owns the RS485 bus. Its `rs485_bus` task, pinned to core 1, is the **only** task that touches the UART/Modbus master; DCMD writes and Modbus-TCP requests (`gw_mbtcp`) are queued to it. The host test `check_rs485` examines this rule: no `uart_*` or `mb_master_*` call outside `gw_bus` and `gw_rs485`, and no call of a `gw_core` function that reaches the Modbus master. The accepted exceptions are in `idf/test/host/rules/rs485_exceptions.txt`, each with its reason.
- `gw_edge`: the `sparkplug` task on core 0. Runs the esp-mqtt client, bdSeq and will handling, backoff and broker failover, and drives `gw_node`.
- `gw_board`: DI sampling, plus the `local_io` task, which is the only writer of the TCA9554 DO expander. Also the LED, buzzer and BOOT button.
- `gw_bsp`: a declarative description of all board pins and polarities. Drivers contain no board data.
- `gw_net`: W5500 Ethernet (openeth in QEMU), Wi-Fi backup, IPv6 and SNTP.
- `gw_storage`: LittleFS config, NVS (encrypted) secrets, bdSeq, the NVS encryption migration and factory reset.
- `gw_web`: the HTTPS UI and REST API, with Basic auth, a CSRF check (`X-Requested-With` plus Origin/Host) and the `web_guard` stall watchdog.
- `gw_fw`: OTA (HTTPS download or web upload), the trial period and rollback.
- `gw_shell`: the `esp_console` shell (`status`, `devices`, `config`, …), which the E2E harness drives over UART0.
- `gw_log`: remote syslog.
- `gw_ap`: the commissioning SoftAP.
- `main`: wires everything together. Its modes are normal, recovery (config rejected: nothing started, recovery network and web only) and commissioning.

### Concurrency rules

- Producers never block consumers. Device and DI updates are atomic dirty flags plus an optional wake, and the Sparkplug task reconciles every loop. Anything that must not be lost goes through bounded static queues.
- `gw_tagdb` is the only shared memory, behind a short mutex.
- All cJSON use goes through `gw_port_json_lock()`, because cJSON's hooks and error state are global (#106). The host test `check_jsonlock` examines this rule: no cJSON outside `gw_core`, and in `gw_core` each call path to cJSON passes `gw_port_json_lock()`.
- The network stack, esp-mqtt and the event loop are pinned to core 0, away from the bus on core 1.

### Memory rules (REL-2)

- Limits: 16 devices, 64 tags per device, 256 tags in total.
- **The firmware assumes the 8 MB of PSRAM of the board (#135).** It does not support a board without PSRAM. Do not add fallbacks or smaller sizes for a board without PSRAM.
- All own memory is fixed-size, static or allocated once at boot. It goes to the PSRAM (`gw_storage_malloc`, `gw_storage_calloc`, `gw_storage_big_alloc`, and mbedTLS through them). These functions never use internal RAM when the PSRAM is full: the allocation fails.
- A board whose PSRAM the boot does not detect runs the fault mode only (`CONFIG_SPIRAM_IGNORE_NOTFOUND`; `GW_WEB_FAULT` in `main.c`). The log, the shell and the web UI give the fault. Only the network, the web server and the shell start, from internal RAM. The gateway does not start.
- **No malloc in the polling, encoding or publish path.** The host test `check_noalloc` examines this rule. It examines each function of the files in `idf/test/host/rules/noalloc_path.txt`, and each function of `gw_core` and nanopb that they call. Add a new file of this path to that list, with its reason.
- Internal RAM is still shared with Wi-Fi, lwIP, esp-mqtt, the task stacks and DMA. The E2E enforces ≥ 8 KB minimum free and a ≥ 17 KB largest block at the worst-case config validation (#80). The check runs in the PSRAM variant, the configuration of the board (`tls` scenario, node GW29).
- **Never lower these thresholds.** Measure any change in `.bss`, stack or heap with the `E2E_PSRAM=1 --only tls` / #80 checks.

### QEMU-only code

- `CONFIG_GW_IO_SIMULATED` replaces GPIO, the I²C expander and the LED with RAM models and enables emulator-only shell commands (`di`, `storm`, `validation hold`, `panic`, `pubfail`, `claimsim`, …). It must never be enabled on hardware.
- `CONFIG_GW_EMU_NO_PSRAM` (hidden) is set only in the QEMU build without PSRAM (`sdkconfig.qemu`: `CONFIG_GW_IO_SIMULATED` and no `CONFIG_SPIRAM`). There the own memory goes to internal RAM, with a 6 KB payload buffer and a store & forward ring of 512 samples. A build without `CONFIG_SPIRAM` and without `CONFIG_GW_IO_SIMULATED` does not compile.
- `CONFIG_GW_FW_PARK_OTHER_CORE` parks core 1 while flash is mapped, to work around QEMU MMU races (#107, #111). `gw_bus_hold()` makes the bus task step aside at a lock-free point first.

## E2E harness (`idf/tools/e2e/e2e.py`)

This single large file runs every scenario. It:
- isolates itself in a network namespace when it can;
- writes its own `config.json` into the flash image (`tools/e2e/fs/`);
- runs a private mosquitto (plus TLS, misbehaving and relay brokers), the pymodbus slave (units 1–2 respond, 3–5 never do), SNTP and syslog;
- asserts on the console log, the shell, MQTT payloads and HTTP responses.

Rules that tests and fixes must respect:
- Only known emulator faults are retried: QEMU process death, or the image-check PIF panic. Firmware failure lines (Guru Meditation, assert, task_wdt, CORRUPT HEAP, stack overflow) and hangs are never retried.
- Discarded attempts are recorded in `results.json` (`retries`, `remeasured`).
- The OTA scenarios run single-threaded TCG (`E2E_TCG_THREAD=auto`), because MTTCG crashes QEMU under load (`docs/upstream-reports.md` §7–8). This makes them 2–7× slower with PSRAM.
- The PSRAM variant is the main variant (#135). Its full run has every scenario except the three OTA trial scenarios (#97, #99/#101, #112): they examine logic, not memory, and they run only in the faster variant without PSRAM (`DEFAULT_VARIANT_SCENARIOS` in `scenarios.py`). Put a new scenario that depends on memory in the PSRAM variant. `test_harness.py` checks that each scenario is in one variant and in one CI group.
- Timing flakes are fixed by waiting on evidence (progress, log lines) up to a bound, never by loosening what a check asserts.
- `test_harness.py` unit-tests the retry, hang and progress logic with fake clocks. Extend it when changing that logic.

## Security constraints

- Never burn eFuses on a real device, and never run `espefuse` against one. QEMU's emulated eFuse file is fine.
- No build in `main` may burn an eFuse on the board (#133). A Kconfig option that burns eFuses (`CONFIG_SECURE_BOOT`, `CONFIG_SECURE_FLASH_ENC_ENABLED`, `CONFIG_SECURE_DISABLE_ROM_DL_MODE`, `CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE`, `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK`, `CONFIG_BOOT_ROM_LOG_ALWAYS_OFF`/`_ON_GPIO_*`, `CONFIG_NVS_ENCRYPTION` with the HMAC scheme) can appear only in a QEMU overlay. The host test `check_efuse_options` examines `sdkconfig.defaults` and each overlay that is not for QEMU. When you change this list, change the list in `idf/test/host/check_efuse_options.cmake` too.
- Signing keys for tests are generated into build or scratch dirs (e.g. `build-qemu/e2e_signing_key.pem`) and are never committed.

## Conventions

- Commits reference issues with `Fixes #N` or `Refs #N`. Feature and fix branches are merged into `main` with `--no-ff`.
- When behaviour or measurements change, update the documents in `docs/` (in STE): `docs/reference/tests.md` has the latest results and the test counts, `docs/reference/known-limitations.md` the known limitations, and `docs/reference/e2e-harness.md` the notes on the test environment. Keep only the current values: no history of runs. The root `README.md` changes only when a feature or the status of the project changes.
- Managed components are pinned with `==` in each `idf_component.yml` and hashed in `dependencies.lock`. Host-test dependencies (Unity, cJSON) are pinned by commit hash via FetchContent.
- A change can update a managed component that writes data (`littlefs`) or ESP-IDF (the NVS format, `otadata`, the app descriptor, the partition table). Such a change must pass the downgrade test (#148). Run `E2E_DOWNGRADE_REF=<the last release> tools/e2e/run_e2e.sh --only downgrade` in both variants. Before the tag `v1.0`, use the commit before the change. The earlier image must start in normal mode with the same configuration and secrets. If it cannot, merge the change only when the pre-flight check refuses that downgrade (`nvs_format` and the LittleFS disk version in the capabilities block, `docs/reference/firmware-updates.md`).
