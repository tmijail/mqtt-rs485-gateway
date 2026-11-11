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
| Commit | `main` 8c2e8be |
| Date | 2026-11-11 |

The harness ran in its own network namespace (`--privileged`). No other E2E ran at the same time, except where the table says "in parallel".

## The variants (issue #135)

Since issue #135, the full run of each variant has different scenarios:

- **PSRAM** (`E2E_PSRAM=1`): all the scenarios except the three of the default variant. It has the memory configuration of the board.
- **Default** (no PSRAM): only the three OTA scenarios `ota-park`, `ota-wiring` and `ota-trial`. They examine logic, not memory, and they are 4 to 7 times slower with PSRAM.

Thus the durations of this document are not comparable with the durations before #135. The measurement of `main` eb7c870 (2026-11-07), when each variant ran all the scenarios, gave 1 093 s (default) and 2 347 s (PSRAM) for the full runs one at a time.

## One-time setup

Measured on 2026-11-07.

| Step | Duration |
|---|---|
| `docker pull` of `espressif/idf:v6.1` (first time) | 398 s |
| `docker build -t gw-e2e idf/tools/e2e` | 19 s |
| `git clone` into the volume | 2 s |

## CI groups

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

## Duration of each scenario

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

## Optional scenarios

| Scenario | Variant | Result | Duration |
|---|---|---|---|
| `tck` (§4.7 of `idf/README.md`) | PSRAM | 19/19 checks passed, one test repeated (`PrimaryHostTest`, MQTT 3.1.1: no TCK result) | 406.9 s for the 14 tests and the repeated test |

The `umh` scenario did not run in this measurement.

## Notes

- FW-1 OTA with PSRAM took 584.5 s. That is 65 % of its 900 s limit. The OTA scenarios use single-threaded TCG (issue #107), so the speed of one host thread sets their duration. On a CPU with P-cores and E-cores, Windows can put that thread on an E-core. If a run fails on this limit, add `E2E_TIMEOUT_SCALE=1.5`.
- Since issue #144, the self-check installs a new image at 60 s after its boot, at the earliest. The scenarios that install images take more time than before #144. Without PSRAM, the #97 integration took 99.6 s on eb7c870 and 293.9 s now. With PSRAM, `issue #94 OTA to NVS encryption` took 103.3 s on eb7c870 and 201.4 s now.
