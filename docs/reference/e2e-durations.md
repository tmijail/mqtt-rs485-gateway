# E2E durations

This document gives the measured durations of the QEMU E2E tests in Docker on a Windows computer. The procedure is in [Run the E2E tests in Docker](../how-to/run-e2e-in-docker.md).

## Test computer

| Item | Value |
|---|---|
| CPU | Intel Core i9-12900K (8 P-cores, 8 E-cores, 24 threads) |
| Memory | 32 GB (the Docker VM: 24 CPUs, 15.5 GB) |
| Operating system | Windows 11 Pro 10.0.26300 |
| Docker | Docker Desktop, engine 29.1.3, WSL 2 |
| Image | `gw-e2e` (`idf/tools/e2e/Dockerfile`, on `espressif/idf:v6.1@sha256:f81b6e07…`) |
| QEMU | `esp_develop_9.2.2_20260417` |
| Commit | `main` eb7c870 |
| Date | 2026-11-07 |

The harness ran in its own network namespace (`--privileged`). No other E2E ran at the same time, except where the table says "in parallel".

## One-time setup

| Step | Duration |
|---|---|
| `docker pull` of `espressif/idf:v6.1` (first time) | 398 s |
| `docker build -t gw-e2e idf/tools/e2e` | 19 s |
| `git clone` into the volume | 2 s |

## Builds

The builds ran from a clean volume, with all 24 CPUs (`JOBS` not set).

| Build | Duration |
|---|---|
| Default variant: `build-qemu` and `build-qemu-plain` (`E2E_STAGE`) | 69 s |
| PSRAM variant: `build-qemu-psram` (`build-qemu-plain` already built) | 39 s |

## Full runs, one at a time

Each run is `run_e2e.sh` on the images of the build above (`E2E_PREBUILT`), with no other E2E at the same time.

| Variant | Result | Retries | QEMU exits | Duration |
|---|---|---|---|---|
| Default | 181/181 checks passed | none | none | 1 093 s (18.2 min) |
| PSRAM | 187/187 checks passed | none | none | 2 347 s (39.1 min) |

The two runs one after the other, with the two builds, took 3 548 s (59 min).

## CI groups in parallel

`run_ci_groups.sh` with `PARALLEL=5`: the build of one variant, then the five CI groups at the same time. The duration includes the incremental build. The PSRAM duration goes from the start of the container to the last result file of a group.

| Variant | Result | Retries | QEMU exits | Duration |
|---|---|---|---|---|
| Default | 201/201 checks passed (181 as one full run) | none | none | 392 s (6.5 min) |
| PSRAM | 207/207 checks passed (187 as one full run) | none | none | 736 s (12.3 min) |

The scenario time of each group, in seconds (the sum of `durations` in its `results.json`):

| Group | `--only` | Default | PSRAM | Longest scenario (PSRAM) |
|---|---|---|---|---|
| `ota-wiring` | `network,ota-wiring` | 104 | 710 | #97 integration, 701 s |
| `ota-fw1` | `ota-fw1` | 145 | 431 | FW-1 OTA, 431 s |
| `ota-trial` | `ota-park,ota-trial` | 108 | 382 | #99/#101 trial, 343 s |
| `nvs-main` | `nvs,main` | 363 | 361 | main, 191 s |
| `other` | `failsafe,web,reset,tls,failover,mqtt5,phost,sf,https,slow` | 365 | 471 | sf spill, 129 s |

The longest group sets the duration of a parallel run. With PSRAM, that is `ota-wiring`, because of the #97 integration.

## Duration of each scenario (full runs)

The values come from `results.json` → `durations`, in seconds. The harness gives each scenario a limit of 900 s (`DEFAULT_BUDGET_S`).

| Scenario | Default | PSRAM |
|---|---|---|
| failsafe | 48.6 | 48.0 |
| network | 9.2 | 9.2 |
| web | 20.6 | 19.1 |
| reset | 36.4 | 36.4 |
| tls | 52.1 | 51.3 |
| failover | 39.2 | 39.3 |
| mqtt5 | 2.6 | 3.6 |
| max_packet | 15.0 | 16.0 |
| phost | 15.9 | 15.8 |
| sf | 13.1 | 12.6 |
| sf spill | 12.0 | 136.4 |
| stack margin | 19.4 | 19.5 |
| https | 65.8 | 65.2 |
| FW-1 OTA | 144.3 | 418.6 |
| issue #112 park in a long RS485 transaction | 28.7 | 32.7 |
| issue #97 OTA trial wiring | 99.6 | **706.7** |
| issues #99/#101 OTA trial | 83.3 | 336.1 |
| issues #67/#94 NVS migration | 60.7 | 60.8 |
| issue #109 full unencrypted NVS | 6.7 | 5.6 |
| issue #94 OTA to NVS encryption | 108.2 | 103.3 |
| slow | 17.4 | 17.4 |
| main | 191.3 | 190.9 |
| **Sum** | **1 090.1** | **2 344.5** |

## Notes

- With PSRAM, the #97 integration took 706.7 s alone and 701.1 s in parallel. That is 78–79 % of its 900 s limit. `idf/README.md` §4.5 gives 477 s and 496 s for this scenario on the machine of those measurements. The load of the parallel groups did not change the duration. The OTA scenarios use single-threaded TCG (issue #107), so the speed of one host thread sets their duration. On a CPU with P-cores and E-cores, Windows can put that thread on an E-core. The project did not examine the cause. If a run fails on this limit, add `E2E_TIMEOUT_SCALE=1.5`.
- The parallel groups are 2.8 times (default) and 3.2 times (PSRAM) faster than the full runs. Use them for the check of a change. The two variants with their builds took 1 128 s (19 min), against 3 548 s (59 min) one at a time.
- The other scenarios have approximately the same durations in the two variants. The differences come from the OTA scenarios and `sf spill` (the microSD spill runs only with PSRAM).
