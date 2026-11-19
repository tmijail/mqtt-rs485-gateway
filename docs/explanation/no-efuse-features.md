# Features that burn eFuses

This document tells why the firmware in `main` burns no eFuse, what the archive branch keeps, and what it means for the security of a gateway. It also gives the two items of issue #133 that are still open.

## The decision

**Decision of issue #133 (option 3):** the project does not burn eFuses on the board. These features are not in `main`:

- Secure Boot V2 with RSA-3072 (FW-2);
- flash encryption in release mode (FW-3);
- NVS encryption with an HMAC key in the eFuse block KEY5 on the board (the eFuse part of CFG-7);
- the serial download mode, disabled permanently (`CONFIG_SECURE_DISABLE_ROM_DL_MODE`).

## The reason

This is an open-source project for a board that anyone can buy. Each of these features burns eFuses at the first boot, and an eFuse burn is permanent. After the burn, only the person with the signing key can change the firmware. The owner of the board cannot read or flash the board through USB.

This is a vendor lock. The project does not lock the devices of their owners. The owner of the board must always be able to flash it, read it and change it.

## The archive branch

The branch `archive/efuse-production` keeps these features as they were in `main` at commit 4a0e01c. It contains:

- the overlay `sdkconfig.production` and the partition table `partitions-production.csv`;
- `tools/sign_firmware.sh` (the offline signature and its checks), `tools/check_production.sh` and `tools/e2e/secure_boot_qemu.sh`;
- the CI job `firmware-production`;
- the build checks of the production options (issues #93 and #108 B7) and their host tests;
- §3.7 and §6.12 of its `idf/README.md`: the factory procedure, the management of the signing key and the known limitations.

The project does not maintain that branch. Do not put its features in `main` again. Do not add other features that burn eFuses, for example the DS peripheral for the MQTT client key.

> **Warning: Do not flash a build of the archive branch on a board that you want to use again.** Its first boot burns eFuses, and you cannot undo this. Nobody tested that branch on a real board.

## The default build burns no eFuse

The project examined the default build (`idf.py set-target esp32s3 build`, ESP-IDF v6.1) for eFuse writes:

- The `sdkconfig` has no option that burns an eFuse. These options are off: `CONFIG_SECURE_BOOT`, `CONFIG_SECURE_FLASH_ENC_ENABLED`, `CONFIG_SECURE_DISABLE_ROM_DL_MODE`, `CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE`, `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` and `CONFIG_NVS_ENCRYPTION`.
- The ROM log mode stays `CONFIG_BOOT_ROM_LOG_ALWAYS_ON`, the ESP-IDF default. The other ROM log modes burn an eFuse at the first boot.
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` (FW-1) uses only `otadata`. It is not the anti-rollback feature, which burns an eFuse counter.
- The app and the bootloader contain no function that writes eFuses (`esp_efuse_write_*`, `esp_efuse_utility_burn_*`, `efuse_hal_program`). `xtensa-esp32s3-elf-nm` finds only read functions, for example for the MAC address and the chip version.

A Kconfig option that burns eFuses can appear only in a QEMU overlay. These options are `CONFIG_SECURE_BOOT`, `CONFIG_SECURE_FLASH_ENC_ENABLED`, `CONFIG_SECURE_DISABLE_ROM_DL_MODE`, `CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE`, `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK`, `CONFIG_BOOT_ROM_LOG_ALWAYS_OFF` and `CONFIG_BOOT_ROM_LOG_ON_GPIO_*`, and `CONFIG_NVS_ENCRYPTION` with the HMAC scheme.

## The two open items of #133

The QEMU build (`sdkconfig.qemu`) still has two features that issue #133 must decide. Each one uses the emulated eFuse file of the run. Nothing real is burned.

- **Signed OTA images without Secure Boot** (`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`). `esp_ota_end()` verifies the RSA-3072 signature of each image with the public key of the running app. This burns no eFuse, also on a board. `tools/e2e/run_e2e.sh` makes the test key in `build-qemu/`.
- **NVS encryption** with the HMAC key in KEY5, and the migration from an unencrypted NVS (issues #67 and #94). Refer to [Commissioning and secrets](../reference/commissioning-and-secrets.md).

Until #133 decides, the project adds no work that depends on these two features.

> **Warning: Never flash the QEMU build on a board.** On a real chip, its first boot burns the HMAC key into KEY5 for the NVS encryption, and you cannot undo this. Also, the QEMU build simulates the I/O.

## What this means for the security of a gateway

Without Secure Boot and flash encryption, physical access to the board gives full control of it:

- A person with access to the USB-C port can read all of the flash. The flash contains the secrets in NVS (not encrypted), the files of LittleFS (the MQTT client key of MQTT-5, the certificates) and the core dump. If the secrets are important, put the gateway in a locked cabinet.
- That person can also flash a different firmware through USB. The owner can do this too, for example after a failed update.
- The default build does not verify a signature on OTA images. The gateway accepts each image of this project (`project_name`) with a correct checksum and SHA-256. The access control of each channel protects the update: the web password, the ACL of the broker for NCMD, and TLS with a server check for the HTTPS download. Refer to [Firmware updates](../reference/firmware-updates.md).
- With PSRAM, the data in PSRAM is in clear text outside the chip. Refer to [Known limitations](../reference/known-limitations.md).
- `idf.py coredump-info` reads the core dump through USB. Without USB, the web API (with the password) and the shell give it. Refer to [The core dump](core-dump.md).
