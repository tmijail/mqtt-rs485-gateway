# E2E durations

This document gives the measured durations of the QEMU E2E tests:

- [Measurement on a Linux cloud container](#measurement-on-a-linux-cloud-container): the scenarios, the CI groups and the builds, with the scenarios of `main` after issue #233.
- [Measurement on a Windows computer](#measurement-on-a-windows-computer): the one-time setup of Docker and the optional scenario `tck`. The procedure is in [Run the E2E tests in Docker](../how-to/run-e2e-in-docker.md).

Compare the measurements of one computer only. The speed of one host thread sets the duration of the OTA scenarios, because they use single-threaded TCG (refer to [Single-threaded TCG](e2e-harness.md#single-threaded-tcg-in-the-ota-scenarios-107)).

## Measurement on a Linux cloud container

### Test computer

| Item | Value |
|---|---|
| CPU | Intel Xeon @ 2.10 GHz, 4 CPUs (1 thread per core) |
| Memory | 15 GB |
| Operating system | Ubuntu 24.04.5 LTS (a cloud container) |
| ESP-IDF | v6.1, installed with `install.sh esp32s3` (no Docker) |
| QEMU | `esp_develop_9.2.2_20260417` |
| Commit | The branch of #233 |
| Date | 2026-11-18 |

All runs used `run_ci_groups.sh` with prebuilt images, `E2E_TIMEOUT_SCALE=1.5` and two groups at the same time. Each harness ran in its own network namespace.

CAUTION: Set `E2E_TIMEOUT_SCALE=1.5` on a computer of this speed. A scenario has a limit of 900 s (`DEFAULT_BUDGET_S`), and `FW-1 OTA by web upload` takes more than half of it.

### Duration of each scenario

The values come from `results.json` → `durations`, in seconds.

| Scenario | Variant | Duration |
|---|---|---|
| FW-1 OTA by NCMD | PSRAM | 375.9 |
| FW-1 OTA by web upload | PSRAM | **556.4** |
| issue #135 no PSRAM | PSRAM | 13.4 |
| issues #67/#94 NVS migration | PSRAM | 69.9 |
| issue #109 full unencrypted NVS | PSRAM | 8.2 |
| issue #94 OTA to NVS encryption | PSRAM | 202.4 |
| main | PSRAM | 234.5 |
| failsafe | PSRAM | 58.3 |
| network | PSRAM | 26.1 |
| web | PSRAM | 82.0 |
| reset | PSRAM | 154.4 |
| tls | PSRAM | 125.1 |
| failover | PSRAM | 40.3 |
| mqtt5 | PSRAM | 7.8 |
| max_packet | PSRAM | 16.0 |
| phost | PSRAM | 25.7 |
| sf | PSRAM | 20.2 |
| sf spill | PSRAM | 173.8 |
| stack margin | PSRAM | 20.5 |
| https | PSRAM | 113.9 |
| time | PSRAM | 56.4 |
| slow | PSRAM | 23.8 |
| **Sum (PSRAM)** | | **2 405.0** |
| issue #112 park in a long RS485 transaction | Default | 35.6 |
| issue #97 OTA trial wiring | Default | 420.3 |
| issue #144 OTA self-check | Default | 194.6 |
| **Sum (default)** | | **650.5** |

- The longest step of the FW-1 scenarios is a web upload of 1 MB with PSRAM: approximately 220 to 250 s. `FW-1 OTA by NCMD` has one (the parks of #111), and `FW-1 OTA by web upload` has two (#215 and the trial of `e2e-v3`). Thus `FW-1 OTA by web upload` is the longest scenario, and it sets a lower limit for the longest group.
- During the trial of `e2e-v3`, the other-slot button got `testing e2e-v3: 24 s left`, and the configuration save got `22 s left`. Thus the actions of the trial used approximately 8 s of the self-check time of 30 s.
- An image that rejects the configuration went back to the earlier image 2.8 s after its boot. The check requires less than 15 s (half of the self-check time).

### CI groups

The PSRAM variant ran two groups at the same time (`PARALLEL=2`), and then the default variant ran its group.

| Variant | Retries | QEMU exits | Duration |
|---|---|---|---|
| PSRAM | none | none | 1 324 s (22.1 min) |
| Default | none | none | 636 s (10.6 min) |

The scenario time of each group, in seconds:

| Variant | Group | `--only` | Scenario time | Longest scenario |
|---|---|---|---|---|
| PSRAM | `ota-fw1-web` | `ota-fw1-web,nopsram` | 567.0 | FW-1 OTA by web upload, 553.4 s |
| PSRAM | `ota-fw1-ncmd` | `ota-fw1-ncmd,reset,slow` | 553.9 | FW-1 OTA by NCMD, 374.6 s |
| PSRAM | `nvs-main` | `nvs,main` | 510.2 | main, 233.3 s |
| PSRAM | `sf-tls` | `sf,tls,mqtt5,network,failover` | 415.5 | sf spill, 162.1 s |
| PSRAM | `other` | `failsafe,web,phost,https,time` | 329.3 | https, 109.4 s |
| Default | `ota-trial` | `ota-park,ota-wiring,ota-trial` | 632.8 | #97 integration, 411.8 s |

The CI runs all groups at the same time, so the longest group sets the duration of the E2E jobs.

### Builds with ccache

The measurement used one cache directory and the same build directory path for each build, as the CI does. `CCACHE_COMPILERCHECK=content` was set (refer to [The caches of the CI](tests.md#the-caches-of-the-ci)).

| Build | Empty cache | Full cache | Hits |
|---|---|---|---|
| Firmware (`idf.py set-target esp32s3 build`, the `firmware` job) | 91 s | 36 s | 1 075 of 1 077 |
| QEMU images of the default variant (`E2E_STAGE`, `build-qemu` and `build-qemu-plain`) | 179 s | 70 s | 2 148 of 2 152 |

The venv of the harness took 4 s to make on this computer, and QEMU uses 71 MB.

### The caches in the CI

These are the durations of the steps on the GitHub runners, from run 44353304685 (empty caches) and run 44367214752 (full caches, commit 3b6e3d7):

| Step | Empty caches | Full caches |
|---|---|---|
| `firmware`: build | 100 s | 37 s |
| `e2e-build`, PSRAM variant: QEMU images | 80 s (without `build-qemu-plain`) | 63 s (with `build-qemu-plain`) |
| `e2e-build`, default variant: QEMU images | 151 s (with `build-qemu-plain`) | 40 s (without `build-qemu-plain`) |
| `e2e`: install of QEMU, mosquitto and the venv | 11 to 24 s | 5 to 11 s |

The two `e2e-build` rows do not have the same work in the two runs. The start of the container of each job takes 60 to 100 s, and no cache makes it shorter.

The full CI run with full caches took 16.7 min from the push to the last job. The `e2e` groups took 5.6 to 10.4 min each, and `e2e-summary` ended 15.5 min after the push. The PSRAM job of `e2e-poison` (FW-1 with heap poisoning) is the longest job: 15.0 min for its scenarios.

## Measurement on a Windows computer

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

The harness ran in its own network namespace (`--privileged`).

On a CPU with P-cores and E-cores, Windows can put the thread of an OTA scenario on an E-core. If a run fails on a time limit, add `E2E_TIMEOUT_SCALE=1.5`.

### One-time setup

| Step | Duration |
|---|---|
| `docker pull` of `espressif/idf:v6.1` (first time) | 398 s |
| `docker build -t gw-e2e idf/tools/e2e` | 19 s |
| `git clone` into the volume | 2 s |

### Optional scenarios

| Scenario | Variant | Result | Duration |
|---|---|---|---|
| `tck` ([Sparkplug TCK results](sparkplug-tck-results.md)) | PSRAM | 19/19 checks passed, one test repeated (`PrimaryHostTest`, MQTT 3.1.1: no TCK result) | 406.9 s for the 14 tests and the repeated test |
