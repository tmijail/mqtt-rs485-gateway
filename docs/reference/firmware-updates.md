# Firmware updates

This document gives the facts about the firmware updates of the gateway (FW-1): how an update starts, the image checks, the capabilities block, the trial and the rollback, the two slots, safe mode and the known limits.

- For the procedures, refer to [Update the firmware](../how-to/update-the-firmware.md).
- For the routes, refer to [The REST API](rest-api.md#firmware). For the metrics, refer to [Sparkplug messages and metrics](sparkplug-messages-and-metrics.md).
- For the key `ota`, refer to [The configuration document](configuration.md#ota).

> **Warning: Do not use this document as verified data for the board.** The firmware has never run on the board (#1, #18). The E2E examines the updates, the trial and the rollback in QEMU. Nobody has measured the times on the board yet (#128).

## Overview

- The flash has two app slots, `ota_0` and `ota_1`, of 4 MiB each (`idf/partitions.csv`). An update writes the slot that does not run.
- The process of an update and of a rollback comes from #144. The product owner approved it.
- The decision logic is pure C with host tests, in `gw_core`: `gw_ota`, `gw_bootloop`, `gw_fwlast`, `gw_cfgschema`, `gw_lkgnet` and `gw_cfgprev`. The component `gw_fw` applies it, with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`.
- The default build does not require a signature, and it burns no eFuse. The owner of the board can always flash it over USB. Refer to [The features that burn eFuses](../explanation/no-efuse-features.md).

## How an update starts

| Start | Condition |
|---|---|
| The card **Firmware** of the Status tab, or `POST /api/firmware` (a URL, or the image as the body) | Always possible, for a user who knows the web password |
| The NCMD `Node Control/Firmware Update` (String, alias 40) with the `https://` URL of the image | Only with `ota.mqtt: true` (#223, default `false`) |
| The NCMD `Node Control/Firmware SHA-256` (String, alias 44): the SHA-256 for the next `Firmware Update` | Only with `ota.mqtt: true` |
| The button **Restart into `<version>`**, or `POST /api/firmware/other` | Always possible. Refer to [The other slot](#the-other-slot) |
| The NCMD `Node Control/Boot Other Slot` (Boolean `true`, alias 43) | Only with `ota.mqtt: true` (decision of the product owner, #144 and #223) |

**With `ota.mqtt: false`:**

- The NBIRTH does not contain `Node Control/Firmware Update`, `Node Control/Firmware SHA-256` and `Node Control/Boot Other Slot`. The gateway never accepts them, thus they are not metrics that it reports on (`tck-id-topics-nbirth-metric-reqs`). A host then shows no control that the gateway refuses.
- The Sparkplug engine (`gw_node`) discards an NCMD that writes them, by name or by alias. It starts no update and does not boot the other slot. `Firmware Update/State` does not change.
- The log gives the cause: `firmware updates from MQTT are off (ota.mqtt); no update started`, or `…; no boot of the other slot`.
- The specification lets a host send a metric that is not in the NBIRTH, and the Edge Node must be resilient to it (`tck-id-operational-behavior-data-commands-ncmd-metric-name`, a SHOULD).
- `Firmware Update/State` and `Firmware Update/Progress` stay in the NBIRTH, because an update from the web UI changes them.
- A change of `ota.mqtt` needs a save and a restart. Thus a new NBIRTH always agrees with the value.

**Why `Boot Other Slot` needs `ota.mqtt`.** The specification gives no rule for this command, thus the gateway selects its conditions. The image in the other slot can be an earlier image, or an image that failed its trial. Without this condition, each client that can publish to the NCMD topic of the node can change the running firmware to that image.

**The URL.** An update URL has 255 characters at most, through the NCMD and through the web UI (#87). The firmware rejects a longer URL and gives the cause. It never truncates the URL.

**The SHA-256 of the user (#215).**

- The NCMD `Node Control/Firmware SHA-256` gives the SHA-256 of the image file, in the same NCMD as the URL or in an earlier one. The engine starts the update only after it read all the metrics of the NCMD.
- The value is 64 hexadecimal characters, capital or small letters. An empty value removes the check.
- The gateway discards a value that is not valid, with the cause in the log. Then the URL of the same NCMD starts no update (`tck-id-operational-behavior-data-commands-ncmd-metric-value`).
- The web UI and the API give the same check: the key `sha256` of `POST /api/firmware`, or the header `X-Image-SHA256` of an upload.
- After the last byte, and before `esp_ota_end()` or `esp_https_ota_finish()`, `gw_fw` reads the written image back from the flash. It calculates its SHA-256 in PSRAM, 4 KB at a time (`gw_sha256`).
- When the two values do not agree, the image does not become the boot image. The state is `failed: image <version>: the SHA-256 of the file does not agree`. An upload gets `422` with the two values.
- The check is a check of the file, not a signature.

**The card Firmware of the web UI (#215).**

- Each update has a confirmation (#193). The choice of a file does not start the upload.
- When the pre-flight check refuses a request, a red box gives each reason on its own line (#192). Its button sends the same request again with force, after a confirmation.
- The browser calculates the progress of an upload. It does not poll the gateway during the upload.
- The trial has no button: the image confirms itself. A rollback shows only in the summary of the card.

## The download

- The download runs in a dedicated task, `fw_update` (8 KB, priority 2, core 0), that exists only during the download. The Sparkplug task does not block.
- The firmware writes the flash sector by sector as the data arrives (`esp_https_ota`). It does not erase the full slot at one time.
- Only `https://`. The CA of the server is `ota.ca_file`. If it is not set, the firmware uses `mqtt.tls.ca_file` when TLS is on. If that is not set, it uses the public CA bundle of ESP-IDF.
- The firmware needs a valid clock to examine the certificate of the server.
- The image must be from this project: the `project_name` of its `esp_app_desc_t`. The firmware examines this before it writes past the header. Then it does the [pre-flight check](#the-pre-flight-check).
- `esp_ota_end()` verifies the full image: the checksum and the SHA-256 of the image. The QEMU build also verifies a signature.
- If a step fails, the running firmware continues, and the state gives the cause.
- If the image is valid, the firmware writes the record `fw_last` and sets the boot partition. Then it publishes `restarting into <version>` and does an orderly restart (NDEATH).
- An interrupted download does not continue: it starts again from the start. The state gives the bytes that arrived: `failed: download of <version> interrupted after N bytes`.
- A web upload runs in the task of the web server, and blocks the web UI until it ends.
- While the firmware writes the flash, the other tasks can see short delays. Each erase of a sector stops the cache for some ms. The Modbus polling did not stop in the E2E. Nobody has measured this on the board.

## The pre-flight check

The check runs before the firmware writes past the header of the image: the first 320 B of an upload, the first KB of a download, or the head of the other slot for the button (#144, #148).

**The configuration schema.** Each configuration key and enum value has a "since" schema number (`gw_config.c`, table `k_since`). The firmware reads schema 2 (`GW_CONFIG_SCHEMA`). For the list of the keys of schema 2, refer to [The configuration schema](configuration.md#the-configuration-schema). When the firmware loads or saves the configuration, it calculates the schema that the document needs (`gw_cfgschema`).

The firmware refuses the image (HTTP 409 for an upload and for the button, `failed: …` in the state) when:

- the stored configuration needs a higher schema than the image reads. The refusal lists the JSON paths that are too new, for example `the configuration needs schema 2, the image reads 1: <path>=<value> (2)`;
- the image has a lower NVS format (either byte) than the running image;
- the image has another major LittleFS disk version, or a lower minor version;
- the image has no capabilities block of version 2 (all images before #144). Such an image is "unknown". The web UI tells that force is necessary.

**Force.**

- The web UI and the API permit force: `"force": true` in the JSON, or `?force=1` with an image or with `/api/firmware/other`. In the web UI, the check box **Force** is only for a download.
- A forced image must reach recovery mode or better. Refer to [The expected mode](#the-expected-mode).
- The NCMD has no force.
- Force does not override the check of the encrypted NVS (#102).

> **Caution: Do not force an image with a lower NVS format or LittleFS version, unless you accept the loss of the NVS or of `/cfg`.** Such an image cannot read the data, and it can erase it.

When the running image cannot load the stored configuration (recovery mode, safe mode, commissioning), the schema part of the check has nothing to compare, and it passes. The image must then reach recovery mode or better.

## The capabilities block

Each image declares its capabilities in a block of 32 B immediately after its `esp_app_desc_t`: section `.rodata_custom_desc`, offset 288 in the image.

| Field | Content |
|---|---|
| Magic | `GWCP` |
| Version, size | Version 2 since #144 |
| Flag *encrypted NVS* | `CONFIG_NVS_ENCRYPTION` |
| Flag *encrypted flash* | Only for information. No build in `main` sets it |
| Schema (version 2) | The highest configuration schema that the image reads (`GW_CONFIG_SCHEMA`) |
| NVS format (version 2) | High byte: the page format of the IDF NVS (0xFF minus `NVS_CONST_NVS_VERSION`). Low byte: the format of the NVS keys and records of the gateway (`GW_NVS_RECORDS`) |
| LittleFS disk version (version 2) | `LFS_DISK_VERSION` |

The build reads `NVS_CONST_NVS_VERSION` and `LFS_DISK_VERSION` from the sources of the libraries (`gw_fw/CMakeLists.txt`). Thus an update of a library changes the block too. A change that updates `littlefs` or ESP-IDF must pass the downgrade test (#148). Refer to [Tests](tests.md).

### Images that would lose the encrypted NVS

This check applies only to a gateway with an encrypted NVS, thus only to the QEMU build (#133).

- When the NVS is encrypted, or a migration waits, the firmware rejects an image that does not declare an encrypted NVS, also with force (#102). That image could not read the NVS and would erase it, and a later rollback would also find it erased.
- The rejection occurs when the header arrives, much before `esp_ota_set_boot_partition()`: `failed: image <version> refused: built without NVS encryption (this device's NVS is encrypted)`.
- An image without the block counts as an image that does not declare an encrypted NVS. Thus a gateway with an encrypted NVS cannot go back by OTA to an image from before the block.
- While the NVS stays unencrypted (an image on trial, safe mode, no LittleFS), or in a build without encryption, the firmware accepts all images.

## The trial of a new image

A new image boots on trial (`ESP_OTA_IMG_PENDING_VERIFY`). The trial is a **self-check of the gateway**. It does not use the broker, the Modbus slaves, NTP or the update server.

The firmware confirms the image at the first instant when all of these are true:

- The uptime is 60 s or more (`CONFIG_GW_FW_SELFCHECK_S`; 30 s in the QEMU build). The uptime comes from the FreeRTOS tick of this boot, because in QEMU the timer of `esp_timer` does not restart at a software reset.
- The gateway is in its [expected mode](#the-expected-mode).
- The network driver started. The link and the broker are not necessary.
- The loop of the web server answered the ping of `web_guard` in the last 5 s. This item applies only when a web server runs.

The rules:

- To confirm the image is to write `otadata` (`esp_ota_mark_app_valid_cancel_rollback()`). The `main` task does the check approximately one time each second.
- If the write fails, the image stays on trial. The firmware tries the write again after 1, 2, 4 … up to 60 s, and one more time before a planned restart (#99).
- **Backstop.** If the self-check is not true at 5 min (`CONFIG_GW_FW_BACKSTOP_S`), the firmware rejects the image with the cause `self-check failed: <item>`. The task watchdog does not watch a blocked `app_main`. Thus an `esp_timer` rejects the image 10 s after the backstop if the check did not run.
- **Rejection.** The firmware writes the cause to `fw_last`, then does a planned restart (NDEATH). The image is still pending, thus the bootloader boots the earlier image.
- **Any reset during the trial** (a crash, a watchdog, a power cut, a restart) makes the bootloader boot the earlier image.
- **During the trial, the firmware refuses** configuration saves, updates and the restart into the other slot: HTTP 409, `testing <version>: N s left`. A factory reset stays permitted. A configuration save does not confirm the image.

## The expected mode

The modes are normal, recovery and commissioning. For the modes, refer to [Commissioning and secrets](commissioning-and-secrets.md#the-modes).

- An image installed from **normal mode** must reach normal mode. If it rejects the configuration, the firmware rejects the image at once, with the JSON path (`configuration rejected: <path>`). Thus a fleet with nobody near it keeps its MQTT connection.
- An image installed from recovery mode, from safe mode, from commissioning, or with force, must reach recovery mode or better. Then it stays, and the operator corrects the configuration in the web UI.
- The firmware stores the expected mode in `fw_last` before `esp_ota_set_boot_partition()`. An image that boots on trial without a record of its own update must reach recovery mode or better.

## The boot-loop counter and safe mode

The counter is in RTC memory that a reset does not clear (`RTC_NOINIT`, with a magic number and a CRC). The firmware updates it at the start of `app_main`, before `gw_storage_init()`.

| Reset reason | Counter |
|---|---|
| Panic, interrupt watchdog, task watchdog, other watchdog, CPU lock-up | +1 |
| Power-on, brownout, power glitch, a bad magic number or CRC | 0 |
| Software (a planned restart), the reset button, other reasons | No change |

- A pending image ignores the counter: its trial protects it.
- The counter goes to 0 after 120 s of uptime in normal mode (`CONFIG_GW_FW_BOOTLOOP_CLEAR_S`). It also goes to 0 at a restart that the operator asks for: the shell, the web UI, the NCMD Reboot, a configuration save.
- At 3, the gateway boots in **safe mode**: recovery mode with the cause "repeated crashes".
- In safe mode, the network, the web UI (also with `web.enabled: false`), the shell and the firmware upload run. Modbus, Sparkplug, Modbus TCP, the spill to the card and the NVS migration do not run. The firmware does not load the configuration. The network uses the [recovery network](#the-recovery-network).
- Safe mode ends after 60 min (`CONFIG_GW_FW_SAFE_MODE_MIN`) with a restart that tries normal mode again. The counter is then 2, thus one more crash starts safe mode again. A restart by the operator or a power cycle also ends safe mode.
- There is no automatic boot of the other slot. It could do a downgrade across data formats that nobody examined.
- The shell (line `bootloop`) and `GET /api/firmware` (`boot_count`, `safe_mode`) show the counter.

**Safe mode on request (#209).** `POST /api/restart?safe=1`, the shell command `restart safe` and the button **Restart into safe mode** set a request in the same RTC record.

- The next boot takes the request, for one boot only, and starts safe mode with the cause "request". The counter does not change.
- A power-on, a brownout or a power glitch removes the request.
- The firmware refuses the request during an update, during the trial of a new image (the restart would roll it back) and in safe mode.
- `GET /api/status` gives `safe_mode` with the cause and the time left.

## The last result

One NVS string, `fw_last` (namespace `gw`, 95 characters at most), keeps the result of the last update: `r=<result>;m=<n|r>;v=<version>;c=<cause>`.

- It is never a raw struct. Each image reads it, and a reader ignores the keys that it does not know.
- The firmware writes it before each step that can end in a restart: the boot partition of an update, or the rejection of a new image. It also writes each other result.
- The facts that cross images come from this record, the reset reason and `otadata`. The RTC memory keeps only the boot-loop counter of the running image, because its address can change between images.
- Each NBIRTH publishes the result in `Firmware Update/State`. The web UI and the shell show it.
- The cause in `fw_last` is cut to 95 characters. The log has the full path.

## The other slot

The web UI shows the two slots: the running slot, and the other slot with its version and its state (`previous`, `set to boot`, `rejected` with its cause, `invalid`, or `empty`).

- The button **Restart into `<version>`**, `POST /api/firmware/other` and the NCMD `Node Control/Boot Other Slot` (only with `ota.mqtt: true`) do the pre-flight check. Then they call `esp_ota_set_boot_partition()` (it verifies the image) in the task `fw_update`, and do a planned restart.
- The other image boots on trial, also when its trial failed before. `esp_ota_set_boot_partition()` writes a new `otadata` entry over an aborted one.
- The firmware refuses it during a trial or a transfer (`409`), and when the other slot is empty or failed its verification in this boot (`404`).
- The cause of a rejection stays in `fw_last`, because `esp_ota_get_last_invalid_partition()` forgets it when `otadata` changes.

## The configuration from before the update

A downgrade often needs the document that the earlier image read (#146). The decision logic is in `gw_cfgprev` (host test `test_cfgprev`).

- **Arm.** When the self-check confirms a new image, the firmware arms the record with the version of the image in the other slot. That is the image that ran before the update.
- **Capture.** The first save that changes the stored document first copies the stored document to `/cfg/config.prev.json`. Later saves do not change the copy.
- The NVS record `cfg_prev` (namespace `gw`) is a string: `a=<armed version>;v=<version>;s=<schema>;t=<time>;h=<hash>`. It holds the version of the earlier image and the schema of its document. It also holds the time of the copy (0 without a valid clock) and a hash (FNV-1a 32) of the new document.
- **No capture** occurs when the stored document is not one that the running image accepted: recovery mode, the embedded default, no file. The record is then disarmed.

**Downgrade.** This is an upload, a download or the button **Restart into `<version>`** to the version of the record. When the only reason of a refusal is the stored configuration, the firmware compares the hash of `config.json` with the hash of the record:

- **The same hash:** the configuration did not change since the capture. After the image is verified, the firmware restores `config.prev.json`, and keeps `config.json` as `config.next.json`. The pre-flight check passes with the schema of the earlier document. If the boot partition cannot be set, `config.next.json` becomes `config.json` again. The earlier image must reach normal mode.
- **Another hash:** the refusal also gives the JSON paths that differ between the two documents, for example `the configuration from before the update to this firmware is not restored: it changed since (network.hostname)`. The secrets do not count. Force installs the image into recovery mode.

**Restore in recovery mode.** `GET /api/firmware` gives `cfg_prev` (version, time, schema). The card **Firmware** shows "Configuration from before the update: `<version>`, saved on `<date>`". In recovery mode, it also shows the button **Restore the configuration from before the update** (`PUT /api/config?restore_prev=1`). The firmware validates the earlier document as at the boot, with the stored secrets. Then it writes the document and restarts. A document that the running image rejects gets `400` with the JSON path.

**The choice (MAY).** The firmware restores the earlier document only when the image cannot read the stored configuration. If the earlier image can read it, the changes after the update stay. Thus a downgrade does not remove a change that the earlier image accepts.

**Secrets.** The secrets stay in NVS, not in the documents.

- A restore keeps the current secrets. A password that the operator changed after the update stays in use.
- A secret key that a later version adds stays in NVS. The earlier image reads its secrets by key name, thus it ignores the new key. A new update to the later version finds it again. A factory reset erases it.
- Limit: if a later version makes a secret longer than the earlier image reads, the earlier image reads it as empty (`nvs_get_str` refuses the length).

**Only one level.** A new confirmed update arms the record again, and its first change replaces `config.prev.json`. If the earlier image fails its trial after a restore, the later image runs with the earlier document. Its own document stays in `config.next.json` until the next restore.

## The state

`Firmware Update/State` (String, alias 41) and `Firmware Update/Progress` (UInt32 %, alias 42) are in the NBIRTH, and in an NDATA when they change (each 10 %). The shell (`status`, line `ota`) and `GET /api/firmware` also give the details.

| State | Meaning |
|---|---|
| `idle` | No update is recorded for the running image |
| `downloading`, `downloading <v>` | A transfer (download or upload) is in progress |
| `restarting into <v>` | The image is set to boot, or the new image was rejected: restart |
| `testing <v>` | The new image runs on trial |
| `installed <v>` | The self-check confirmed the new image. It stays after later restarts |
| `failed: <cause>` | The last update did not take effect: a refused request, a transfer or verification failure, an interrupted update |
| `rolled back from <v>: <cause>` | The earlier image runs again |

- The causes of a rollback are `restarted during the test (<reset reason>)`, `self-check failed: <item>`, `configuration rejected: <path>` and `no memory to start`.
- The items of the self-check are `mode not reached`, `network driver not started`, `web server not alive` and `main task blocked`.
- `Firmware Update/State` has 95 characters at most (`GW_NODE_FW_STATE_MAX`). The firmware publishes a longer state truncated, with a warning in the log. The full reason (up to 239 characters) is in the shell (`status`) and in `GET /api/firmware`. This limit keeps the Sparkplug engine, a single allocation, in the heap class of 21 504 B (#80).

## The recovery network

Recovery mode, safe mode and commissioning start the network with the network of the last valid configuration (`lkg_net` in NVS).

- The firmware writes `lkg_net` only from a confirmed image in normal mode, after the confirmation.
- The format accepts new fields: `GWLN`, a version, then one record for each field (tag, length, value). A reader takes the tags that it knows and ignores the others.
- A field that is not there keeps its default (DHCP). Thus a rollback into recovery mode keeps the static IP of a site.

## Rollback and the accepted risk

- **Manual rollback.** The developers test a release, so that it does not break MQTT. MQTT can fail after an update because of the setup of the site or of the broker. Then use the button **Restart into `<version>`**, or upload the earlier image.
- **Accepted risk (#144).** An image that passes the self-check, but cannot connect to the broker of a site, stays installed until a person uses the web UI. The gateway cannot see a difference between a broker that is down and a firmware that cannot connect. For fleets, update a pilot gateway first, and then update the others in stages.
- **Last resort.** If the web UI is not available in any mode, flash the firmware over USB. Refer to [Build the firmware](../how-to/build-the-firmware.md). The owner of the board always keeps this access, because the project burns no eFuses (#133).

## Known limits

- **A power cut during the trial also causes a rollback.** The bootloader cannot see a difference between a power cut and a crash. `fw_last` gives `rolled back from <v>: restarted during the test (power-on)`.
- **The times are not measured on the board.** #128 must measure the self-check items and a web upload, to confirm `CONFIG_GW_FW_SELFCHECK_S` (60 s) and `CONFIG_GW_FW_BOOTLOOP_CLEAR_S` (120 s).
- **The bootloader does not reserve the RTC memory of the boot-loop counter.** The E2E shows that the counter survives the panic resets in QEMU. #149 must reserve the memory before the first flash of a board.
- **The text limits (#87).** A text of a Sparkplug metric has 256 characters at most (`SPB_STRING_MAX`). A text of a property or of a DataSet cell has 63 at most. The firmware rejects a longer text with a log, and never truncates it. Thus it does not apply such an NCMD or DCMD, and does not publish such a message of its own. The limit costs 193 B of stack for each metric in the encoder and in the decoder (`Payload_Metric` of nanopb).
- **There is no anti-rollback of the version** (`CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK`). The gateway accepts each valid image of the project that passes the pre-flight check, also an older one. The anti-rollback burns an eFuse counter, and the project burns no eFuse (#133).
- **If the firmware can never write `otadata`**, the image continues on trial, and the firmware tries again each 60 s. Saves and updates stay refused. A power cut, or a planned restart after one last attempt, causes a rollback (#99).
- **Flash writes outside the polling path.** The confirmation of an image (it maps `otadata` with `esp_partition_mmap`), and the writes of `fw_last` and `lkg_net`, occur one time, in the `main` task. The backstop timer writes `fw_last` from the `esp_timer` task, one time. These writes take some ms, far from the 15 s of the watchdog.
- **Images from before #144 are "unknown"** for the pre-flight check. They need force, and then they must reach recovery mode or better.

## Test hooks of the QEMU build

These hooks exist only with `CONFIG_GW_IO_SIMULATED`:

- The file `/cfg/.sim-reject-config` makes a new image on trial reject the stored configuration (path `e2e.simulated`). The E2E uses it to examine the rejection of an image installed from normal mode, because all the images that the E2E builds read the same schema.
- The QEMU build counts the key `network.hostname` as the schema after the schema of the build (`GW_CONFIG_SCHEMA` + 1, `k_sim_since` in `gw_storage.c`) (#146). The other keys count as schema 1 in this build.
- The E2E reads the schema of the image under test from its capabilities block. It writes the block of a copy of the image again, to declare the next schema. The images are the same binary, thus the E2E examines the decision and the restored document, not a real older parser.
- The self-check time is 30 s. For the durations, refer to [E2E durations](e2e-durations.md).
