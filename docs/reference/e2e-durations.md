# E2E durations

This document gives the measured durations of the QEMU E2E tests on two computers:

- [Measurement of issue #233](#measurement-of-issue-233): a Linux cloud container with 4 CPUs. It compares `main` 21f721d with the changes of #233 on the same computer.
- [Measurement on a Windows computer](#measurement-on-a-windows-computer): Docker on a Windows computer with 24 threads, on `main` 8c2e8be. The procedure is in [Run the E2E tests in Docker](../how-to/run-e2e-in-docker.md).

Do not compare the durations of the two computers directly. On the cloud container, the baseline scenarios took 1.1 to 2.5 times longer than on the Windows computer: for example FW-1 OTA 1.7 times, the #97 integration 1.9 times, the #144 self-check 1.1 times and `tls` 2.5 times. Compare the measurements of one computer only.

## Measurement of issue #233

Issue #233 makes the full E2E shorter with three changes:

- The QEMU build sets the self-check time of a new image to 30 s (`CONFIG_GW_FW_SELFCHECK_S`). The board keeps 60 s.
- FW-1 OTA is two scenarios with their own boot: `FW-1 OTA by NCMD` and `FW-1 OTA by web upload`. The PSRAM variant has five CI groups.
- The CI uses ccache and keeps QEMU and the venv of the harness in `actions/cache`.

### Test computer

| Item | Value |
|---|---|
| CPU | Intel Xeon @ 2.10 GHz, 4 CPUs (1 thread per core) |
| Memory | 15 GB |
| Operating system | Ubuntu 24.04.5 LTS (a cloud container) |
| ESP-IDF | v6.1, installed with `install.sh esp32s3` (no Docker) |
| QEMU | `esp_develop_9.2.2_20260417` |
| Commits | `main` 21f721d (the baseline), and the branch of #233 on it |
| Date | 2026-11-18 |

All runs used `run_ci_groups.sh` with prebuilt images, `E2E_TIMEOUT_SCALE=1.5` and two groups at the same time. The baseline and the branch ran at the same time, one group each, so they had the same load. Each harness ran in its own network namespace.

CAUTION: Set `E2E_TIMEOUT_SCALE=1.5` on a computer of this speed. Without it, the baseline FW-1 OTA (1 006.8 s) goes past the limit of 900 s for one scenario.

### Duration of each scenario: baseline and #233

The values come from `results.json` → `durations`, in seconds.

| Scenario | Variant | `main` 21f721d | #233 |
|---|---|---|---|
| FW-1 OTA | PSRAM | 1 006.8 | — |
| FW-1 OTA by NCMD | PSRAM | — | 375.9 |
| FW-1 OTA by web upload | PSRAM | — | 556.4 |
| issue #135 no PSRAM | PSRAM | 13.5 | 13.4 |
| issues #67/#94 NVS migration | PSRAM | 68.2 | 69.9 |
| issue #109 full unencrypted NVS | PSRAM | 8.3 | 8.2 |
| issue #94 OTA to NVS encryption | PSRAM | 234.2 | 202.4 |
| main | PSRAM | 234.5 | 234.5 |
| failsafe | PSRAM | 58.6 | 58.3 |
| network | PSRAM | 26.2 | 26.1 |
| web | PSRAM | 78.1 | 82.0 |
| reset | PSRAM | 157.2 | 154.4 |
| tls | PSRAM | 127.3 | 125.1 |
| failover | PSRAM | 40.2 | 40.3 |
| mqtt5 | PSRAM | 7.5 | 7.8 |
| max_packet | PSRAM | 15.7 | 16.0 |
| phost | PSRAM | 25.8 | 25.7 |
| sf | PSRAM | 19.8 | 20.2 |
| sf spill | PSRAM | 172.7 | 173.8 |
| stack margin | PSRAM | 20.5 | 20.5 |
| https | PSRAM | 112.0 | 113.9 |
| time | PSRAM | 55.1 | 56.4 |
| slow | PSRAM | 23.9 | 23.8 |
| **Sum (PSRAM)** | | **2 506.1** | **2 405.0** |
| issue #112 park in a long RS485 transaction | Default | 34.5 | 35.6 |
| issue #97 OTA trial wiring | Default | 571.2 | 420.3 |
| issue #144 OTA self-check | Default | 302.3 | 194.6 |
| **Sum (default)** | | **908.0** | **650.5** |

The scenario time of the two variants went from 3 414.1 s to 3 055.5 s: 358.6 s (6.0 min) less.

- The scenarios that wait for a self-check are shorter: the #97 integration by 150.9 s (5 self-checks), the #144 self-check by 107.7 s, and the OTA to NVS encryption by 31.8 s.
- FW-1 OTA was 1 006.8 s. Its two scenarios together are 932.3 s: 3 self-checks less, and one more boot. The other scenarios did not change.
- The longest step of FW-1 OTA is a web upload of 1 MB with PSRAM: approximately 220 to 250 s. `FW-1 OTA by NCMD` has one (the parks of #111), and `FW-1 OTA by web upload` has two (#215 and the trial of `e2e-v3`). Thus `FW-1 OTA by web upload` is the longest scenario, and it sets a lower limit for the longest group.
- During the trial of `e2e-v3`, the other-slot button got `testing e2e-v3: 24 s left`, and the configuration save got `22 s left`. Thus the actions of the trial used approximately 8 s of the 30 s.
- An image that rejects the configuration went back to the earlier image 2.8 s after its boot. The check requires less than 15 s (half of the self-check time).


### CI groups of #233

The final run used the groups of #233 (`CI_GROUPS`: five PSRAM groups and one default group) on the images of the branch. The PSRAM variant ran two groups at the same time (`PARALLEL=2`), and then the default variant ran its group.

| Variant | Result | Retries | QEMU exits | Duration |
|---|---|---|---|---|
| PSRAM | 284/284 checks passed (264 as one full run) | none | none | 1 324 s (22.1 min) |
| Default | 18/18 checks passed | none | none | 636 s (10.6 min) |

The scenario time of each group, in seconds:

| Variant | Group | `--only` | Scenario time | Longest scenario |
|---|---|---|---|---|
| PSRAM | `ota-fw1-web` | `ota-fw1-web,nopsram` | 567.0 | FW-1 OTA by web upload, 553.4 s |
| PSRAM | `ota-fw1-ncmd` | `ota-fw1-ncmd,reset,slow` | 553.9 | FW-1 OTA by NCMD, 374.6 s |
| PSRAM | `nvs-main` | `nvs,main` | 510.2 | main, 233.3 s |
| PSRAM | `sf-tls` | `sf,tls,mqtt5,network,failover` | 415.5 | sf spill, 162.1 s |
| PSRAM | `other` | `failsafe,web,phost,https,time` | 329.3 | https, 109.4 s |
| Default | `ota-trial` | `ota-park,ota-wiring,ota-trial` | 632.8 | #97 integration, 411.8 s |

On the same computer before #233, the longest PSRAM group was `ota-fw1` with 1 020.3 s, and the default group took 908.0 s. The CI runs all groups at the same time, so the longest group sets the duration of the E2E jobs: 633 s instead of 1 020 s on this computer.

### Builds with ccache

The CI jobs that build use ccache since #233. The measurement used one cache directory and the same build directory path for each build, as the CI does. `CCACHE_COMPILERCHECK=content` was set (refer to [The caches of the CI](tests.md#the-caches-of-the-ci)).

| Build | Empty cache | Full cache | Hits |
|---|---|---|---|
| Firmware (`idf.py set-target esp32s3 build`, the `firmware` job) | 91 s | 36 s | 1 075 of 1 077 |
| QEMU images of the default variant (`E2E_STAGE`, `build-qemu` and `build-qemu-plain`) | 179 s | 70 s | 2 148 of 2 152 |

With the default check of ccache (`mtime`), the second build had 0 hits of 1 077: each configuration writes a new `-specs` file into the build directory.

The venv of the harness took 4 s to make on this computer, and QEMU uses 71 MB. In the last CI run with results (2026-11-06), the full install step of an E2E job took 13 to 14 s, and the start of its container approximately 100 s. The CI has not run since 2026-11-06 (a billing problem of the account), so there is no CI measurement of the caches yet.

## Measurement on a Windows computer

This measurement is from before issue #233. It has one FW-1 OTA scenario and three groups in the PSRAM variant.

### Test computer

| Item | Value |
|---|---|
| CPU | Intel Core i9-12900K (8 P-cores, 8 E-cores, 24 threads) |
| Memory | 32 GB (the Docker VM: 24 CPUs, 15.5 GB) |
| Operating system | Windows 11 Pro 10.0.26300 |
| Docker | Docker Desktop, engine 29.1.3, WSL 2 |
| Image | `gw-e2e` (`idf/tools/e2e/Dockerfile`, on `espressif/idf:v6.1@sha256:f81b6e07…`) |
| QEMU | `esp_develop_9.2.2_20260417` |
| Commit | `main` 8c2e8be |
| Date | 2026-11-11 |

The harness ran in its own network namespace (`--privileged`). No other E2E ran at the same time, except where the table says "in parallel".

### The variants (issue #135)

Since issue #135, the full run of each variant has different scenarios:

- **PSRAM** (`E2E_PSRAM=1`): all the scenarios except the three of the default variant. It has the memory configuration of the board.
- **Default** (no PSRAM): only the three OTA scenarios `ota-park`, `ota-wiring` and `ota-trial`. They examine logic, not memory, and they are 4 to 7 times slower with PSRAM.

Thus the durations of this document are not comparable with the durations before #135. The measurement of `main` eb7c870 (2026-11-07), when each variant ran all the scenarios, gave 1 093 s (default) and 2 347 s (PSRAM) for the full runs one at a time.

### One-time setup

Measured on 2026-11-07.

| Step | Duration |
|---|---|
| `docker pull` of `espressif/idf:v6.1` (first time) | 398 s |
| `docker build -t gw-e2e idf/tools/e2e` | 19 s |
| `git clone` into the volume | 2 s |

### CI groups

`run_ci_groups.sh` builds the images of one variant, then runs the CI groups of that variant. The PSRAM variant ran its three groups at the same time (`PARALLEL=3`). The default variant has one group. The two variants ran one after the other. The build was incremental (ccache, with earlier builds in the volume). The duration goes from the start of the build to the last result file of a group.

| Variant | Build | Result | Retries | QEMU exits | Duration |
|---|---|---|---|---|---|
| PSRAM | approximately 60 s | 223/223 checks passed (213 as one full run) | none | none | 674 s (11.2 min) |
| Default | approximately 40 s | 15/15 checks passed | none | none | 632 s (10.5 min) |

The two variants with their builds took 1 306 s (21.8 min).

The scenario time of each group, in seconds (the sum of `durations` in its `results.json`):

| Variant | Group | `--only` | Scenario time | Longest scenario |
|---|---|---|---|---|
| PSRAM | `ota-fw1` | `ota-fw1,nopsram` | 596.1 | FW-1 OTA, 584.5 s |
| PSRAM | `nvs-main` | `nvs,main` | 469.8 | main, 200.9 s |
| PSRAM | `other` | `failsafe,network,web,reset,tls,failover,mqtt5,phost,sf,https,time,slow` | 610.2 | sf spill, 153.5 s |
| Default | `ota-trial` | `ota-park,ota-wiring,ota-trial` | 590.9 | #97 integration, 293.9 s |

The longest group sets the duration of a run of the groups. The groups of the two variants have approximately the same scenario time, so the CI jobs end at approximately the same time.

### Duration of each scenario

The values come from `results.json` → `durations` of the groups above, in seconds. The scenarios of the PSRAM variant ran in parallel with the two other groups of that variant. The harness gives each scenario a limit of 900 s (`DEFAULT_BUDGET_S`).

| Scenario | Variant | Duration |
|---|---|---|
| failsafe | PSRAM | 49.1 |
| network | PSRAM | 8.0 |
| web | PSRAM | 26.9 |
| reset | PSRAM | 68.5 |
| tls | PSRAM | 50.5 |
| failover | PSRAM | 39.3 |
| mqtt5 | PSRAM | 6.1 |
| max_packet | PSRAM | 14.9 |
| phost | PSRAM | 21.9 |
| sf | PSRAM | 12.1 |
| sf spill | PSRAM | 153.5 |
| stack margin | PSRAM | 18.3 |
| https | PSRAM | 65.8 |
| time | PSRAM | 57.9 |
| FW-1 OTA | PSRAM | **584.5** |
| issue #135 no PSRAM | PSRAM | 11.6 |
| issues #67/#94 NVS migration | PSRAM | 60.8 |
| issue #109 full unencrypted NVS | PSRAM | 6.7 |
| issue #94 OTA to NVS encryption | PSRAM | 201.4 |
| slow | PSRAM | 17.4 |
| main | PSRAM | 200.9 |
| **Sum (PSRAM)** | | **1 676.1** |
| issue #112 park in a long RS485 transaction | Default | 28.7 |
| issue #97 OTA trial wiring | Default | 293.9 |
| issue #144 OTA self-check | Default | 268.3 |
| **Sum (default)** | | **590.9** |

### Optional scenarios

| Scenario | Variant | Result | Duration |
|---|---|---|---|
| `tck` ([Sparkplug TCK results](sparkplug-tck-results.md)) | PSRAM | 19/19 checks passed, one test repeated (`PrimaryHostTest`, MQTT 3.1.1: no TCK result) | 406.9 s for the 14 tests and the repeated test |

The `umh` scenario did not run in this measurement.

### Notes

- FW-1 OTA with PSRAM took 584.5 s. That is 65 % of its 900 s limit. The OTA scenarios use single-threaded TCG (issue #107), so the speed of one host thread sets their duration. On a CPU with P-cores and E-cores, Windows can put that thread on an E-core. If a run fails on this limit, add `E2E_TIMEOUT_SCALE=1.5`.
- Since issue #144, the self-check installs a new image at 60 s after its boot, at the earliest. The scenarios that install images take more time than before #144. Without PSRAM, the #97 integration took 99.6 s on eb7c870 and 293.9 s now. With PSRAM, `issue #94 OTA to NVS encryption` took 103.3 s on eb7c870 and 201.4 s now.
