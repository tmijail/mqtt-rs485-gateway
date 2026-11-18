# Build configuration

This document gives the facts about the build: the supported ESP-IDF versions, the `sdkconfig` files and overlays, the partition table, the check of the `sdkconfig` and the size of the image. For the procedures, refer to [Build and flash the firmware](../how-to/build-the-firmware.md).

## Supported ESP-IDF versions

The firmware supports ESP-IDF 6.x. The project builds and tests with v6.1, and CI uses the image `espressif/idf:v6.1`.

`gw_edge.c` stops the build with `#error` outside 6.x. The reason: `gw_edge` chains its TLS verification onto the callback that `esp_crt_bundle_attach()` installs (issue #77). Before you extend the range, make sure that this function still installs its verification with `mbedtls_ssl_conf_verify()`.

## The `sdkconfig` files

The build reads `sdkconfig.defaults`, then the overlays of `SDKCONFIG_DEFAULTS` in their sequence. For each file, it also reads the `.esp32s3` variant if it exists. The last value of an option wins. All files are in [`idf/`](../../idf/).

| File | Build | What it sets |
|---|---|---|
| `sdkconfig.defaults` | All builds. Alone, it is the build for the board | The board (16 MB QIO flash, 8 MB octal PSRAM, `partitions.csv`), the task watchdog (REL-1), the core dump to flash, the RS485 UART ISR in IRAM (MB-2), the network stack on core 0, TLS 1.2 and 1.3 with dynamic buffers (MQTT-4, #80), MQTT 5 (MQTT-8), HTTPS (#64), the OTA rollback (FW-1) and the microSD names without LFN (#122). No option burns an eFuse |
| `sdkconfig.qemu` | All QEMU builds | `CONFIG_GW_IO_SIMULATED`, OpenCores Ethernet, no PSRAM, DIO flash, no Wi-Fi, no RS485 hardware half-duplex mode. Also the NVS encryption with the HMAC key in KEY5 and the signed OTA images without Secure Boot (both open in #133), the spill of store & forward to LittleFS, `CONFIG_GW_FW_PARK_OTHER_CORE` (#107) and a self-check of 30 s (#233) |
| `sdkconfig.qemu-psram` | QEMU with PSRAM, after `sdkconfig.qemu` | The octal PSRAM, software SHA and AES (the QEMU GDMA does not reach PSRAM, #85), 10 RX buffers of OpenCores Ethernet (#110) and a store & forward ring of 1 000 samples (#124) |
| `sdkconfig.qemu-plain` | QEMU without NVS encryption, after `sdkconfig.qemu` | `CONFIG_NVS_ENCRYPTION=n`. The E2E uses it as the image before an update to the encrypted build (#94) |
| `sdkconfig.qemu-poison` | QEMU with heap poisoning, after `sdkconfig.qemu` (and `sdkconfig.qemu-psram`) | `CONFIG_HEAP_POISONING_COMPREHENSIVE` (#104) |

Each option in these files has a comment with its reason. Read the comment before you change an option.

> **Warning: Never flash a QEMU build on the board.** Its first boot on a real chip burns the HMAC key into eFuse KEY5, and you cannot undo this. The QEMU build also simulates the I/O. Refer to [Features that burn eFuses](../explanation/no-efuse-features.md).

There is no production overlay in `main`. The overlay `sdkconfig.production`, which burns eFuses, is on the branch `archive/efuse-production` (#133).

## The build folders

| Folder | `SDKCONFIG_DEFAULTS` |
|---|---|
| `build` | `sdkconfig.defaults` (the default) |
| `build-qemu` | `sdkconfig.defaults;sdkconfig.qemu` |
| `build-qemu-psram` | `sdkconfig.defaults;sdkconfig.qemu;sdkconfig.qemu-psram` |
| `build-qemu-plain` | `sdkconfig.defaults;sdkconfig.qemu;sdkconfig.qemu-plain` |
| `build-qemu-poison`, `build-qemu-psram-poison` | the QEMU chain, then `sdkconfig.qemu-poison` |

`tools/e2e/run_e2e.sh` makes these builds. It deletes the `sdkconfig` of a folder when a defaults file is newer. It also makes the test signing key `build-qemu/e2e_signing_key.pem`. The project never commits this key.

## The partition table

`partitions.csv` is for the 16 MB flash of the ESP32-S3-WROOM-1U-N16R8.

| Name | Type | Offset | Size |
|---|---|---|---|
| `nvs` | data, nvs | `0x9000` | 24 KB |
| `otadata` | data, ota | `0xf000` | 8 KB |
| `phy_init` | data, phy | `0x11000` | 4 KB |
| `ota_0` | app | `0x20000` | 4 MB |
| `ota_1` | app | `0x420000` | 4 MB |
| `storage` | data, LittleFS | `0x820000` | 1 MB |
| `coredump` | data, coredump | `0x920000` | 64 KB |

`ota_0` and `ota_1` are the two slots of the OTA update (FW-1). Refer to [Firmware updates](firmware-updates.md). `idf.py flash` writes the folder `idf/fs/` into `storage` (`GW_FS_DIR`). The design of the core dump is in [The core dump](../explanation/core-dump.md).

## How the defaults apply to an existing `sdkconfig`

- The defaults files are dependencies of the configuration (#83). When one of them changes, the next `idf.py build` does the configuration again.
- In that configuration, kconfgen applies a new default only to a value with the mark `# default:`. It does not change a value from `menuconfig`, or a value in a file without these marks.
- An option that is new in the defaults gets into the `sdkconfig` only through such a configuration.

Thus an old `sdkconfig` can keep old values. The project has no installed gateway, and it has no automatic migration of the `sdkconfig`. Make a new `sdkconfig` instead: refer to [Build and flash the firmware](../how-to/build-the-firmware.md#make-a-new-sdkconfig-after-a-change-of-the-defaults). CI always makes a new `sdkconfig`.

## The `sdkconfig` check

[`cmake/gw_sdkconfig_check.cmake`](../../idf/cmake/gw_sdkconfig_check.cmake) runs in each configuration step. It compares the `sdkconfig` in use with the defaults chain of that build.

### The required options

`GW_SDKCONFIG_REQUIRED` holds the options that the firmware needs:

| Option | Why |
|---|---|
| `MBEDTLS_DYNAMIC_BUFFER` | An idle TLS session keeps no record buffers in internal RAM (#80) |
| `MBEDTLS_CUSTOM_MEM_ALLOC` | The mbedTLS allocations go to PSRAM (MQTT-4, #135) |
| `UART_ISR_IN_IRAM` | The UART releases the RS485 DE line also while the cache is off (MB-2) |
| `ESP_TASK_WDT_EN`, `_INIT`, `_TIMEOUT_S`, `_PANIC`, `_CHECK_IDLE_TASK_CPU0`, `_CHECK_IDLE_TASK_CPU1` | The task watchdog on the critical tasks and the idle tasks (REL-1) |
| `BOOTLOADER_APP_ROLLBACK_ENABLE` | A new image that does not confirm itself goes back to the earlier image (FW-1) |
| `ESP_COREDUMP_ENABLE_TO_FLASH` | The core dump goes to the `coredump` partition (REL-1, #132) |
| `SPIRAM`, `SPIRAM_IGNORE_NOTFOUND` | The firmware assumes the PSRAM of the board. A board without detected PSRAM boots only to report the fault (#135) |
| `LOG_MAXIMUM_LEVEL_VERBOSE` | The debug and verbose lines stay in the image, so that a tag can get more lines at run time (#199) |
| `FATFS_LFN_NONE` | FatFs allocates no name buffer on each microSD file operation (#122, REL-2) |
| `LWIP_SNTP_MAX_SERVERS` | The SNTP client gets all the servers of `ntp.servers` (#201) |
| `GW_FW_SELFCHECK_S` | The QEMU build confirms a new image after 30 s (#233) |

When the firmware starts to depend on a new option, add it to `GW_SDKCONFIG_REQUIRED`.

### The behavior of the check

- When a required option has a value that is different from the defaults chain, the configuration fails. The error gives the list, for example `CONFIG_MBEDTLS_DYNAMIC_BUFFER: sdkconfig has n, the defaults y`, and the procedure to make a new `sdkconfig`.
- An overlay that changes a required option on purpose causes no error. The check compares with the value of the overlay.
- The check does not compare an option that is not in the `sdkconfig`. This occurs when the dependencies of the option are off, for example `SPIRAM_IGNORE_NOTFOUND` in the QEMU build without PSRAM.
- With `-D GW_SDKCONFIG_CHECK=warn`, the check gives only a warning.

### The prohibited combination

The check also compares the `sdkconfig` with itself (#103). `CONFIG_NVS_ENCRYPTION=y` without `CONFIG_SECURE_FLASH_ENC_ENABLED` fails outside the emulator build (`CONFIG_GW_IO_SIMULATED`). The reason: the copy of an NVS migration leaves the secrets in clear text in the free blocks of LittleFS. Refer to [Commissioning and secrets](commissioning-and-secrets.md).

Flash encryption is not in `main`. Thus a build for the board with NVS encryption passes the check only with `warn`.

### The host tests of the check

The host tests `sdkconfig_check_*` run the check on files in [`idf/test/host/sdkconfig_check/`](../../idf/test/host/sdkconfig_check/). They cover these cases:

- a current `sdkconfig` (`fresh`), an old one (`stale`) and an old one with `warn`;
- a different watchdog value without an overlay (`wdt5`) and with an overlay (`wdt5_overlay`);
- the prohibited combination: `nvsenc_noflash` fails, `nvsenc_noflash_warn` gives a warning, `nvsenc_sim` passes;
- a build without PSRAM: `nopsram` fails, `nopsram_warn` gives a warning, `nopsram_qemu` passes.

The host test `check_kconfig_names` finds a misspelled `CONFIG_GW_` name. Refer to [Compiler warnings and the stack data](compiler-warnings-and-stack.md#no--wundef-in-the-firmware).

The host test `check_efuse_options` (#228) fails when `sdkconfig.defaults`, one of its `.<target>` variants or an overlay that is not `sdkconfig.qemu*` sets an option that burns an eFuse. The check of the build does not examine these options. Refer to [The rule checks](tests.md#the-rule-checks).

## The size of the image

CI runs `idf.py size` in the job `firmware`. The last result in the documentation is from the release build of issue #227:

| Region | Used |
|---|---|
| Flash code (`.text`) | 1 184 468 B |
| Flash data (`.rodata`) | 601 936 B |
| DIRAM `.bss` | 108 224 B |
| DIRAM `.data` | 25 642 B |
| Total image | 1 897 661 B (45 % of a 4 MB OTA slot) |

IRAM is always 100 % full on the ESP32-S3. This is normal: the other IRAM code goes in DIRAM.

The Wi-Fi driver (NET-4) adds approximately 27 KB of static DIRAM and 347 KB of flash, also when the configuration does not enable Wi-Fi. The project already moves from IRAM to flash all the Wi-Fi code that ESP-IDF permits (`CONFIG_ESP_WIFI_IRAM_OPT=n`, `CONFIG_ESP_WIFI_RX_IRAM_OPT=n`). If you do not need Wi-Fi, set `CONFIG_GW_WIFI=n` to recover this memory.

The memory budget at run time is in [Architecture](architecture.md#memory-rel-2).
