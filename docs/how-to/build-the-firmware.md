# Build and flash the firmware

This guide tells how to install ESP-IDF, build the firmware, flash it on the board and build the QEMU images. It also tells how to make a new `sdkconfig` after a change of the defaults. The build files, the overlays and the build checks are in [Build configuration](../reference/build-configuration.md).

> **Warning: Do not use this guide as verified data for the board.** The firmware ran only in host tests and in QEMU. Nobody flashed it on a real board (#1, #18).

## Before you start

You need:

- A Linux or macOS computer with Git and Python 3. On Windows, or without a local ESP-IDF, use Docker: refer to [Run the E2E tests in Docker](run-e2e-in-docker.md).
- A clone of this repository.
- For the flash: the board and a USB-C cable.

## Install ESP-IDF

The firmware needs ESP-IDF 6.x. The project uses v6.1. The reason for the limit is in [Build configuration](../reference/build-configuration.md#supported-esp-idf-versions).

1. Clone ESP-IDF v6.1:

   ```bash
   mkdir -p ~/sdk/idf && cd ~/sdk/idf
   git clone --depth 1 --branch v6.1 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git
   ```

2. Install the tools for the ESP32-S3:

   ```bash
   cd esp-idf && IDF_TOOLS_PATH=~/sdk/idf/tools ./install.sh esp32s3
   ```

3. In each new terminal, load the environment:

   ```bash
   export IDF_TOOLS_PATH=~/sdk/idf/tools && . ~/sdk/idf/esp-idf/export.sh
   ```

## Build the firmware

1. Go to the folder `idf/` of the repository.
2. Build the firmware:

   ```bash
   idf.py set-target esp32s3 build
   ```

3. Make sure that the build gives 0 warnings. The CI job `firmware` fails on a warning (QA-2).
4. Optional: show the size of the image with `idf.py size`.

`idf.py set-target` makes a new `sdkconfig` from `sdkconfig.defaults`. A later `idf.py build` keeps that `sdkconfig`.

## Flash the board

> **Caution: Make a backup of the configuration before you flash a gateway that has a configuration.** `idf.py flash` replaces the LittleFS partition with the folder `idf/fs/`. This erases `config.json` and the PEM files on the gateway. The secrets in NVS stay.

> **Warning: Do not flash a QEMU build on the board.** On a real chip, its first boot burns an HMAC key into eFuse KEY5, and you cannot undo this. The reason is in [Features that burn eFuses](../explanation/no-efuse-features.md).

1. Connect the board to the computer with the USB-C cable.
2. Optional: to flash a site configuration, copy its `config.json` into `idf/fs/`. To configure a gateway that runs, use the web UI. Refer to [Change, back up and restore the configuration](change-the-configuration.md).
3. Flash the firmware and open the console. Use the serial port of the board, for example `/dev/ttyACM0` or `COM5`:

   ```bash
   idf.py -p <port> flash monitor
   ```

4. Make sure that the console shows the boot of the gateway.
5. To exit the monitor, push `Ctrl+]`.

`idf.py flash` writes the bootloader, the partition table, the initial `otadata`, the app in `ota_0` and the LittleFS image. The initial `otadata` makes the bootloader start `ota_0`.

To erase all data, also the secrets in NVS, type `idf.py -p <port> erase-flash` before the flash.

To install a new image on a gateway in service, use the OTA update. Refer to [Update the firmware](update-the-firmware.md).

## Build a QEMU image

The E2E script `tools/e2e/run_e2e.sh` builds the QEMU images itself. Use this procedure only to build one QEMU image by hand.

1. Go to the folder `idf/`.
2. Build the image without PSRAM:

   ```bash
   idf.py -B build-qemu -D SDKCONFIG=build-qemu/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.qemu" build
   ```

3. For the variant with PSRAM, use the build folder `build-qemu-psram`. Add `sdkconfig.qemu-psram` at the end of `SDKCONFIG_DEFAULTS`.

These two builds have no NVS encryption and sign no image, as the build for the board (#261).

### Build the secure QEMU image

The secure QEMU build has the NVS encryption and the signed OTA images. Only the secure variant of the E2E uses it (refer to [Build configuration](../reference/build-configuration.md#the-two-functions-of-the-secure-qemu-build)).

1. Go to the folder `idf/`.
2. Make the test signing key, if it is not there. The secure QEMU build signs its OTA images:

   ```bash
   mkdir -p build-qemu
   python -m espsecure generate-signing-key --version 2 --scheme rsa3072 build-qemu/e2e_signing_key.pem
   ```

3. Build the image:

   ```bash
   idf.py -B build-qemu-psram-secure -D SDKCONFIG=build-qemu-psram-secure/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.qemu;sdkconfig.qemu-psram;sdkconfig.qemu-secure" build
   ```

Do not commit the signing key. Keep it in the build folder.

## Make a new `sdkconfig` after a change of the defaults

`sdkconfig.defaults` and the overlays apply fully only when the build creates the `sdkconfig`. Do this procedure after you change a defaults file, after a `git pull` that changes one, or when the build gives an undeclared `CONFIG_GW_*` symbol.

1. Delete the `sdkconfig` of the build folder.
2. Do the configuration again with the same `-B`, `-D SDKCONFIG=` and `-D SDKCONFIG_DEFAULTS=` values.

For the default build:

```bash
rm sdkconfig && idf.py reconfigure
```

For the QEMU build:

```bash
rm build-qemu/sdkconfig && idf.py -B build-qemu -D SDKCONFIG=build-qemu/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.qemu" reconfigure
```

`idf.py set-target esp32s3` also makes a new `sdkconfig`.

> **Caution: Do not keep an old `sdkconfig` that has values from `menuconfig`.** A new default does not replace a value from `menuconfig`. The build can then use an option value that the firmware does not expect.

## Change a checked option on purpose

The configuration step stops when a required option has a value that is different from the defaults. The list of the options is in [Build configuration](../reference/build-configuration.md#the-sdkconfig-check).

- For a permanent change, change the option in the defaults file or in an overlay. The check then accepts the new value.
- For a local trial, add `-D GW_SDKCONFIG_CHECK=warn` to the `idf.py` command. The check then gives only a warning.

> **Caution: Do not commit a change that needs `GW_SDKCONFIG_CHECK=warn`.** CI makes a new `sdkconfig` and uses the strict check.

## Make the stack report

1. Build the firmware.
2. In `idf/`, make the report:

   ```bash
   python3 tools/stack_report.py build > stack.txt
   ```

3. To add a task, give its entry function, for example `--task modbus_tcp=gw_mbtcp.c:server_task`.

The contents of the report and its limits are in [Compiler warnings and the stack data](../reference/compiler-warnings-and-stack.md#the-stack-report).

## Run the static analyzer

The CI job `analyzer` does steps 1 and 2 on each push (#230).

1. In `idf/`, build the firmware with the analyzer in a separate folder:

   ```bash
   idf.py -B build-analyzer -D GW_ANALYZER=ON set-target esp32s3 build
   ```

2. Make sure that the build completes. A result of the analyzer stops the build with `error: ... [-Werror=analyzer-...]`.
3. For each result, read the path of the result in the log. The analyzer gives the events of the path that it followed.
4. If the result is a real error, correct the code.
5. If the result is a false positive, change the code so that the analyzer can follow it. Use only a change that keeps the behaviour.
6. Write a comment next to the change, with the name of the check and the reason.

> **Caution: Do not use the folder `build` for the analyzer.** `GW_ANALYZER` stays in the CMake cache of the folder, and the next build in that folder also uses the analyzer.

> **Caution: Do not hide a result with `#pragma` or with `-Wno-analyzer-*`.** A hidden check also hides the real errors of that type in the file.

The scope and the options of the analyzer are in [The static analyzer](../reference/compiler-warnings-and-stack.md#the-static-analyzer).
