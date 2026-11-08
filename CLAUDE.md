# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware for an RS485 (Modbus RTU) → MQTT Sparkplug B 3.0 gateway on the Waveshare ESP32-S3-POE-ETH-8DI-8DO board, built on ESP-IDF v6.1 (target `esp32s3`). Everything lives in `idf/`. `docs/SPEC.md` is the product spec; requirement IDs such as MB-6, SPB-14 and FW-1 come from it, and code, tests and commits refer to them and to GitHub issue numbers (`#80`, `#107`, …).

**This is an open-source project, not a product to flash, lock and sell.** The firmware runs on a board that anyone can buy, and the owner of the board must always be able to reflash it, read it and change it. Do not design for a vendor that controls the devices of its users:
- The default build must never burn eFuses or take away the owner's access: no Secure Boot, no flash encryption in release mode, and no disabled download mode.
- Do not add work that depends on the optional eFuse-burning overlay `sdkconfig.production` (FW-2, FW-3, the eFuse part of CFG-7). Issue #133 must first decide whether these features stay in the project.
- When a requirement in `docs/SPEC.md` assumes a vendor product (factory stations, locked devices), question it before you implement it.

`idf/README.md` is the authoritative, detailed reference. It covers architecture, task table, memory budget, build, configuration, verification results and known limitations. Read the relevant section before changing a subsystem. The existing README, docs and GitHub issues are in Spanish; all new writing follows **Writing standard** below.

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
- A change of a stored format (the configuration document, NVS keys and records, the microSD card format, RTC memory) needs no migration and no backward compatibility. Document the change in `idf/README.md`, and say in the commit that old data is dropped or reset.
- Do not add code that reads an earlier format only for compatibility.

When the product owner says that the first gateway is installed, tag that commit `v1.0`. From then on, each change of a stored format needs an upgrade path and a downgrade path (see #144, #146, #147 and #148).

## Sparkplug B conformance

The gateway must obey the Eclipse Sparkplug B 3.0.0 specification fully. Do not accept a deviation from it.
- Obey each MUST, MUST NOT, SHOULD and SHOULD NOT statement for an Edge Node. A deviation is a bug, also when a document records it.
- The reference is the normative text with a `tck-id`, in the AsciiDoc chapters at the tag `v3.0.0` of `eclipse-sparkplug/sparkplug`. When an example does not agree with the normative text, obey the normative text.
- When `docs/SPEC.md` does not agree with the Sparkplug specification, the Sparkplug specification is correct. Correct `docs/SPEC.md`.
- Do not change the gateway to accept a host that does not obey the specification (for example, the Sparkplug 2.x STATE of UMH Core). Write the limit in the documentation.
- An option for a specific host (for example, UMH Core) must also obey the specification in each of its settings.
- When the specification permits a choice (MAY), write the choice and the reason in `idf/README.md`.
- When a change concerns a Sparkplug rule, give its `tck-id` in the code comment, the test, the issue and the commit.

## Commands

The host tests and the decode check run from the repo root. The firmware commands are run from `idf/`, after `. $IDF_PATH/export.sh`.

```bash
# Host tests (pure C, no IDF): 27 Unity suites + sdkconfig and guard checks, -Werror
cmake -S idf/test/host -B build-host && cmake --build build-host -j && ctest --test-dir build-host --output-on-failure
ctest --test-dir build-host -R test_poll      # one suite; or run build-host/test_poll directly

# Sparkplug interop: decode generated payloads with the official Tahu proto
mkdir -p /tmp/spb && build-host/spb_dump /tmp/spb && python3 idf/tools/spb_decode.py --check /tmp/spb

# Firmware build (CI requires 0 warnings)
idf.py set-target esp32s3 build && idf.py size

# Production overlay (Secure Boot V2, flash + NVS encryption), signed with a throwaway key in the build dir
tools/check_production.sh build-prod
tools/e2e/secure_boot_qemu.sh /tmp/sb-qemu

# E2E: real firmware in QEMU against mosquitto + a pymodbus RTU slave (~15–25 min; PSRAM ~45 min)
python3 -m venv ~/e2e && ~/e2e/bin/pip install -r tools/e2e/requirements.txt
E2E_PYTHON=~/e2e/bin/python tools/e2e/run_e2e.sh                 # default (no PSRAM)
E2E_PSRAM=1 E2E_PYTHON=~/e2e/bin/python tools/e2e/run_e2e.sh     # octal PSRAM variant
E2E_PYTHON=~/e2e/bin/python tools/e2e/run_e2e.sh --only ota,https   # subset of scenarios
#  --only: failsafe,network,web,reset,tls,failover,mqtt5,phost,sf,https,ota,ota-fw1,ota-park,ota-wiring,
#          ota-trial,nvs,slow,refuse,main (ota = the four ota-* names; an unknown name is an error)
E2E_PYTHON=~/e2e/bin/python tools/e2e/run_ci_groups.sh /tmp/gw-groups   # the CI groups: build once, run in parallel
#  Windows / no local IDF: the same in Docker (idf/tools/e2e/Dockerfile), see docs/how-to/run-e2e-in-docker.md
#  E2E_STAGE=<dir> (build the images into <dir>, run nothing), E2E_PREBUILT=<dir> (run on them, no build),
#  PARALLEL=n (groups at the same time); the groups are CI_GROUPS in tools/e2e/scenarios.py
#  E2E_POISON=1 (heap poisoning, use with --only ota), E2E_PLAIN=0 (skip the no-NVS-encryption build),
#  E2E_TIMEOUT_SCALE / --timeout-scale (slow host), --web-stress N, JOBS=n (build parallelism)

# Harness self-test and lint (both run in CI)
python3 idf/tools/e2e/test_harness.py
python3 -m pyflakes idf/tools/e2e/*.py idf/tools/*.py
```

CI (`.github/workflows/ci.yml`) runs host-tests, firmware, firmware-production, the E2E jobs and e2e-poison on every push, in the pinned `espressif/idf:v6.1` image. The E2E jobs build the QEMU images one time per variant (`e2e-build`). Then they run the groups of `scenarios.py` in parallel jobs (`e2e`), and `e2e-summary` makes sure that the groups of each variant are one full run (#121). A change is done when the host tests pass and the full E2E passes in both the default and `E2E_PSRAM=1` variants. Its results go in `results.json`, `qemu-exits.txt` and the `retries` list.

### sdkconfig pitfalls

- `sdkconfig.defaults` and the overlays only apply when an `sdkconfig` is **created**. After changing them, delete the build dir's `sdkconfig` and reconfigure. For example: `rm build-prod/sdkconfig && idf.py -B build-prod -D SDKCONFIG=build-prod/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.production" reconfigure`.
- A stale `sdkconfig` shows up as undeclared `CONFIG_GW_*` symbols.
- `cmake/gw_sdkconfig_check.cmake` fails the build when required options drift. If the firmware starts depending on a new option, add it to `GW_SDKCONFIG_REQUIRED`.
- Overlays:
  - `sdkconfig.qemu` (the QEMU build, with `CONFIG_GW_IO_SIMULATED`);
  - `sdkconfig.qemu-psram`, `sdkconfig.qemu-plain` (no NVS encryption) and `sdkconfig.qemu-poison`, each stacked on `sdkconfig.qemu`;
  - `sdkconfig.production`.

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

Platform calls go through `gw_port.h`, which has two implementations: `port/gw_port_esp.c` for the firmware and `port/gw_port_posix.c` for host tests. New logic belongs here, with a host test in `idf/test/host/`. The other components are thin IDF adapters around it.

### IDF components (`idf/components/`)

- `gw_bus`: owns the RS485 bus. Its `rs485_bus` task, pinned to core 1, is the **only** task that touches the UART/Modbus master; DCMD writes and Modbus-TCP requests (`gw_mbtcp`) are queued to it.
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
- All cJSON use goes through `gw_port_json_lock()`, because cJSON's hooks and error state are global (#106).
- The network stack, esp-mqtt and the event loop are pinned to core 0, away from the bus on core 1.

### Memory rules (REL-2)

- Limits: 16 devices, 64 tags per device, 256 tags in total.
- All own memory is fixed-size, static or allocated once at boot. It goes to PSRAM when present (`gw_storage_big_alloc`) and to internal RAM otherwise.
- **No malloc in the polling, encoding or publish path.**
- Without PSRAM (the QEMU default build) internal RAM is tight. The E2E enforces ≥ 8 KB minimum free and a ≥ 17 KB largest block at the worst-case config validation (#80).
- **Never lower these thresholds.** Measure any change in `.bss`, stack or heap with the `--only tls` / #80 checks.

### QEMU-only code

- `CONFIG_GW_IO_SIMULATED` replaces GPIO, the I²C expander and the LED with RAM models and enables emulator-only shell commands (`di`, `storm`, `validation hold`, `panic`, …). It must never be enabled on hardware.
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
- The OTA scenarios run single-threaded TCG (`E2E_TCG_THREAD=auto`), because MTTCG crashes QEMU under load (`docs/upstream-reports.md` §7–8). This makes them 2–4× slower with PSRAM.
- Timing flakes are fixed by waiting on evidence (progress, log lines) up to a bound, never by loosening what a check asserts.
- `test_harness.py` unit-tests the retry, hang and progress logic with fake clocks. Extend it when changing that logic.

## Security constraints

- Never run `espefuse`/`espsecure` against a real device or burn eFuses. QEMU's emulated eFuse file is fine.
- Signing keys for tests are generated into build or scratch dirs (e.g. `build-qemu/e2e_signing_key.pem`) and are never committed.

## Conventions

- Commits reference issues with `Fixes #N` or `Refs #N`. Feature and fix branches are merged into `main` with `--no-ff`.
- When behaviour or measurements change, update `idf/README.md` (in STE): §4 has the verification results and test counts, §6 the known limitations, §6.10 the test environment notes.
- Managed components are pinned with `==` in each `idf_component.yml` and hashed in `dependencies.lock`. Host-test dependencies (Unity, cJSON) are pinned by commit hash via FetchContent.
