# The release files and the manifest

This document gives the facts about a release of the firmware. It gives the tag and the version, the files, the format of `manifest.json`, the checks of the script, the release workflow and the copy on the documentation site. For the procedure, refer to [Make a release](../how-to/make-a-release.md).

> **Warning: The firmware has never run on the board (#1, #18).** A release has the images of the build for the board. Nobody flashed these images on a board.

## The tag and the version

A release has a tag of the form `v0.<minor>.<patch>`, for example `v0.2.0`. The version of the firmware is the text of the tag without the `v`. It is the value of `GW_VERSION` in [`idf/CMakeLists.txt`](https://github.com/tmijail/mqtt-rs485-gateway/blob/main/idf/CMakeLists.txt).

The tag `v1.0` is not a release tag. It is for the commit of the first installed gateway, and the product owner makes it. From that tag on, a change of a stored format needs an upgrade path and a downgrade path.

The build has two modes. The CMake option `GW_RELEASE` selects the mode:

| Build | Option | Version of the firmware |
|---|---|---|
| Normal build (the default) | `GW_RELEASE` is off | `<GW_VERSION>+g<hash>`, with the first 8 digits of the commit hash. The version ends with `.dirty` when `idf/` has a change that is not in a commit. Without git, the version is `<GW_VERSION>+unknown` |
| Release build | `-D GW_RELEASE=ON` | `<GW_VERSION>`, without the hash |

With `GW_RELEASE=ON`, the configuration of the build fails in these two conditions:

- git gives no commit for the folder `idf/`;
- the folder `idf/` has a change that is not in a commit. Git does not count the ignored files, for example `build/` and `sdkconfig`.

Thus a release image always comes from one commit. Only the release workflow builds with `GW_RELEASE=ON`. The other CI jobs do the normal build.

The firmware does not read the parts of the version. It shows the version as text in the NBIRTH (`Node Info/Firmware`), in the web UI and in the log. It writes the version into the NVS records `fw_last` and `cfg_prev`. It also compares the version of two images as text, to know which image a record is for. Thus a version without the hash changes no function.

Each release needs a version that no other release has. Two different images with the same version make the records `fw_last` and `cfg_prev` ambiguous. Refer to [The last result](firmware-updates.md#the-last-result) and [The configuration from before the update](firmware-updates.md#the-configuration-from-before-the-update).

## The files of a release

The script [`idf/tools/release/make_manifest.py`](https://github.com/tmijail/mqtt-rs485-gateway/blob/main/idf/tools/release/make_manifest.py) writes six files into one folder. The file names are the same in each release.

| File | Contents |
|---|---|
| `bootloader.bin` | The bootloader of ESP-IDF |
| `partition-table.bin` | The partition table of `idf/partitions.csv`. Refer to [The partition table](build-configuration.md#the-partition-table) |
| `ota_data_initial.bin` | A blank `otadata` partition (each byte is `0xFF`) |
| `rs485_spb_gateway.bin` | The application. It is also the image of an OTA update. Refer to [Update the firmware](../how-to/update-the-firmware.md) |
| `manifest.json` | The place, the size and the SHA-256 of each of the four files above |
| `SHA256SUMS` | The SHA-256 of the five files above, in the format of `sha256sum`. The command `sha256sum -c SHA256SUMS` examines the files |

The SHA-256 is a check of the file, not a signature. It finds a damaged or wrong file.

## What a flash of a release writes and keeps

The manifest has a part for the bootloader, the partition table, `otadata` and the application. It has no part for the other partitions. Thus a flash without an erase keeps the configuration and the secrets of the owner.

| Area of the flash | In the manifest | Effect of a flash |
|---|---|---|
| Bootloader | Part `bootloader` | Replaced |
| Partition table | Part `partition-table` | Replaced |
| `otadata` | Part `otadata`, blank | The bootloader starts `ota_0`, the slot of the application |
| `ota_0` | Part `app` | Replaced with the application |
| `nvs` | No part | The secrets and the other NVS records stay |
| `storage` | No part | `config.json` and the PEM files stay |
| `ota_1` | No part | The image of the other slot stays, but `otadata` does not select it |
| `phy_init`, `coredump` | No part | They stay |

`idf.py flash` also writes `storage.bin`, the LittleFS image of the folder `idf/fs/`, into `storage`. A release leaves this file out on purpose, because it replaces the configuration of the owner.

A release has no configuration. For the modes of a gateway without a configuration, refer to [Commissioning and secrets](commissioning-and-secrets.md).

## The format of `manifest.json`

The format is the format of this project. It is not the format of ESP Web Tools. This example has the offsets of the partition table. `<bytes>` and `<64 hex digits>` are placeholders:

```text
{
  "format": 1,
  "name": "rs485_spb_gateway",
  "version": "0.2.0",
  "chip": "ESP32-S3",
  "flash": { "mode": "dio", "freq": "80m", "size": "16MB" },
  "parts": [
    { "name": "bootloader", "file": "bootloader.bin", "offset": 0,
      "size": <bytes>, "sha256": "<64 hex digits>", "partition": null },
    { "name": "partition-table", "file": "partition-table.bin", "offset": 32768,
      "size": <bytes>, "sha256": "<64 hex digits>", "partition": null },
    { "name": "otadata", "file": "ota_data_initial.bin", "offset": 61440,
      "size": <bytes>, "sha256": "<64 hex digits>", "partition": "otadata" },
    { "name": "app", "file": "rs485_spb_gateway.bin", "offset": 131072,
      "size": <bytes>, "sha256": "<64 hex digits>", "partition": "ota_0" }
  ],
  "keep": [
    { "name": "nvs", "offset": 36864, "size": 24576 },
    { "name": "phy_init", "offset": 69632, "size": 4096 },
    { "name": "ota_1", "offset": 4325376, "size": 4194304 },
    { "name": "storage", "offset": 8519680, "size": 1048576 },
    { "name": "coredump", "offset": 9568256, "size": 65536 }
  ]
}
```

| Key | Type | Value |
|---|---|---|
| `format` | Number | The format number of the manifest: `1` |
| `name` | String | The name of the firmware: the CMake project name `rs485_spb_gateway` |
| `version` | String | The version of the firmware. Refer to [The tag and the version](#the-tag-and-the-version) |
| `chip` | String | The chip of the build, with the name that esptool gives: `ESP32-S3` |
| `flash.mode`, `flash.freq`, `flash.size` | String | The flash settings of the build, from `flash_settings` in `flasher_args.json`. They are the values of the esptool options `--flash-mode`, `--flash-freq` and `--flash-size` |
| `parts` | Array | The files that a flash writes |
| `parts[].name` | String | `bootloader`, `partition-table`, `otadata` or `app` |
| `parts[].file` | String | The name of the file, in the folder of the manifest |
| `parts[].offset` | Number | The offset of the part from the start of the flash, in bytes |
| `parts[].size` | Number | The size of the file, in bytes |
| `parts[].sha256` | String | The SHA-256 of the file: 64 hexadecimal digits in lower case |
| `parts[].partition` | String or `null` | The name of the partition that the part fills. `null` for the bootloader and the partition table, which are not in a partition |
| `keep` | Array | The partitions that a flash of the release does not write |
| `keep[].name`, `keep[].offset`, `keep[].size` | String, number, number | The name, the offset and the size of the partition, in bytes |

All the numbers are decimal JSON numbers.

`flash.mode` is `dio` although `sdkconfig.defaults` selects a QIO flash. ESP-IDF gives `dio` for the header of the bootloader of a QIO build. The bootloader sets the QIO mode later.

### Rules for a tool that reads the manifest

- A tool refuses a manifest with a `format` number that it does not know.
- A tool ignores a key that it does not know. A new key does not change the `format` number.
- A change that a tool of format `1` cannot ignore gets a new `format` number.
- A tool writes each part at its `offset`. It writes nothing into a partition of `keep`, unless the owner asks for a full erase.

The parts are equivalent to this esptool command:

```bash
python -m esptool --chip esp32s3 write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB \
    0x0 bootloader.bin 0x8000 partition-table.bin 0xf000 ota_data_initial.bin 0x20000 rs485_spb_gateway.bin
```

## The checks of the script

`make_manifest.py` makes the manifest from the data of the build. Nobody writes a manifest by hand. The script reads these files:

- `flasher_args.json` in the build folder: the file and the offset of each part, and the flash settings;
- `project_description.json` in the build folder: the project name, the version, the target and the path of the `sdkconfig`;
- the `sdkconfig` of the build;
- the partition table of the build (`partition-table.bin`) and the partition table of the source (`idf/partitions.csv`).

When a check fails, the script gives one error line for each problem and the exit status 1. It then writes no file.

| Check | The script refuses |
|---|---|
| The project | A build of a project that is not `rs485_spb_gateway`. An application whose description (`esp_app_desc_t`) has another name or another version than the build data |
| The chip | A build for a target that is not `esp32s3`. A bootloader or an application whose image header has another chip |
| The build for the board | An `sdkconfig` with `CONFIG_GW_IO_SIMULATED=y` or `CONFIG_GW_EMU_NO_PSRAM=y` (a QEMU build). An `sdkconfig` with an option that burns an eFuse, or with `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y`. A part for an encrypted flash |
| The partition table | A partition table of the build that is not the table of `idf/partitions.csv` |
| The offsets | A part at an offset that is not the offset of its partition: `otadata` for the part `otadata`, `ota_0` for the part `app`. A bootloader that is not at `CONFIG_BOOTLOADER_OFFSET_IN_FLASH`. A partition table that is not at `CONFIG_PARTITION_TABLE_OFFSET` |
| The sizes | A part that is larger than its partition. A part or a partition that ends after the end of the flash |
| No overlap | A part that shares a flash sector of 4 KB with another part, or with a partition of `keep` |
| `otadata` | An `otadata` file that is not blank. A blank `otadata` makes the bootloader start `ota_0` |
| The other files of the build | A file that `idf.py flash` writes and that the script does not know. Only `storage.bin` is left out on purpose |
| The tag (`--tag`, `--release`) | A tag that does not have the form `v0.<minor>.<patch>`, or that is not `v` and `GW_VERSION`. A build whose version is not `GW_VERSION`, for example a normal build with the hash |

The options that burn an eFuse are the options of the host test `check_efuse_options`. Refer to [The `sdkconfig` check](build-configuration.md#the-host-tests-of-the-check) and to [Features that burn eFuses](../explanation/no-efuse-features.md). An image of a QEMU build must never be in a release: the secure QEMU build burns an eFuse key on a real chip.

The script has these options:

| Option | Function |
|---|---|
| `--build-dir <dir>` | The build folder of the build for the board, for example `idf/build` |
| `--out <dir>` | The folder for the six files. It must be empty, or hold only the files of an earlier run |
| `--release` | The build must be a release build of this source: its version is `GW_VERSION` |
| `--tag <tag>` | The same as `--release`. The tag must also be `v` and `GW_VERSION` |
| `--tag <tag> --tag-only` | The script examines only the tag against `GW_VERSION`. It needs no build |
| `--partitions <file>`, `--sdkconfig <file>`, `--cmakelists <file>` | Other files than `idf/partitions.csv`, the `sdkconfig` of the build and `idf/CMakeLists.txt` |

[`idf/tools/release/test_make_manifest.py`](https://github.com/tmijail/mqtt-rs485-gateway/blob/main/idf/tools/release/test_make_manifest.py) is the self-test of the script. It makes a small build folder with fake files, and it examines the manifest of a good build and each refusal. The CI job `host-tests` runs it.

## The release workflow

[`.github/workflows/release.yml`](https://github.com/tmijail/mqtt-rs485-gateway/blob/main/.github/workflows/release.yml) starts on a push of a tag `v0.<minor>.<patch>`. It also starts on request (`workflow_dispatch`), as a dry run.

| Job | What it does |
|---|---|
| `build` | Examines the tag against `GW_VERSION` (not in a dry run). Runs `idf.py -D GW_RELEASE=ON set-target esp32s3 build` in the pinned `espressif/idf:v6.1` image, without a cache. Fails on a `warning:`. Runs `make_manifest.py` with `--tag`, or with `--release` in a dry run. Publishes the six files as the artifact `firmware-release` |
| `publish` | Only for a tag. Examines the files with `SHA256SUMS`. Makes the GitHub pre-release of the tag with the six files (`gh release create --prerelease`) |

- A dry run makes no release and no tag.
- Only the job `publish` has the permission `contents: write`. It runs no code of the repository.
- The job `publish` fails when the tag has a release already. The workflow never replaces a release.
- A release is a GitHub pre-release, because no gateway is installed (before `v1.0`).

The job `firmware` of `ci.yml` also runs `make_manifest.py`, on the normal build of each push. Thus a build that does not agree with `idf/partitions.csv` fails the CI before a release. The artifact `firmware-esp32s3` has the six files of that build in `build/release/`. Their version has the commit hash.

## The copy on the documentation site

A browser cannot download a GitHub Release asset from another site, because GitHub sends no CORS header for the assets. Thus the documentation site has a copy of the files of each release (#219). The install tool in the browser reads the files from this copy, on the same site.

The copy is in the folder `firmware/` of the site, at `https://mijail.ar/mqtt-rs485-gateway/firmware/`:

```text
firmware/
├── index.json
├── v0.2.0/
│   ├── bootloader.bin
│   ├── partition-table.bin
│   ├── ota_data_initial.bin
│   ├── rs485_spb_gateway.bin
│   ├── manifest.json
│   └── SHA256SUMS
└── v0.1.0/
    └── (the same six files)
```

Each folder of a release has the six files of [The files of a release](#the-files-of-a-release), with no change. The copy has no other file of the GitHub release.

### The format of `index.json`

`index.json` is the list of the releases on the site. This example has two releases:

```json
{
  "format": 1,
  "releases": [
    { "tag": "v0.2.0", "version": "0.2.0", "published": "2026-11-30T12:00:00Z",
      "prerelease": true, "manifest": "v0.2.0/manifest.json" },
    { "tag": "v0.1.0", "version": "0.1.0", "published": "2026-11-23T09:00:00Z",
      "prerelease": true, "manifest": "v0.1.0/manifest.json" }
  ]
}
```

| Key | Type | Value |
|---|---|---|
| `format` | Number | The format number of `index.json`: `1` |
| `releases` | Array | The releases on the site, the newest first. An empty array when the repository has no release |
| `releases[].tag` | String | The tag of the release, for example `v0.2.0` |
| `releases[].version` | String | The version of the firmware: the tag without the `v` |
| `releases[].published` | String | The date and the time of the publication on GitHub, in UTC (ISO 8601). It is `publishedAt` of `gh release list` |
| `releases[].prerelease` | Boolean | `true` for a GitHub pre-release |
| `releases[].manifest` | String | The path of `manifest.json` of the release, relative to `index.json` |

- The sequence of `releases` is by the version, the newest first. The script compares the minor number, then the patch number, as numbers. Thus `v0.10.0` is before `v0.9.0`.
- The list has only the releases with a tag `v0.<minor>.<patch>`. It has no draft.
- With no release, the file is `{"format": 1, "releases": []}`. The install tool then tells the user that the site has no release yet.
- A tool refuses an `index.json` with a `format` number that it does not know. It ignores a key that it does not know.
- The files of a part are in the folder of its manifest. Thus the path of a part is the folder of `manifest` and the `file` of the part.

### The checks before the copy

The script [`idf/tools/release/site_releases.py`](https://github.com/tmijail/mqtt-rs485-gateway/blob/main/idf/tools/release/site_releases.py) makes the copy. It reads the list of the releases (the JSON of `gh release list`) and one folder for each release. It examines each release before the copy.

When a check fails, the script gives one error line for each problem and the exit status 1. It then writes no file. The failure stops the build of the site, and thus the publication. A damaged copy never goes on the site.

| Check | The script refuses |
|---|---|
| The release list | A list that is not a JSON array. A tag that is in the list two times. A release without `publishedAt` or `isPrerelease` |
| The files | A release without one of the six files. A file that is a link |
| `SHA256SUMS` | A line that is not in the format of `sha256sum`. A line for a file that is not one of the five files. A file with no line or with two lines. A file whose SHA-256 is not the SHA-256 of its line |
| The manifest | A `format` that is not `1`. A `name` that is not `rs485_spb_gateway`. A `chip` that is not `ESP32-S3`. A `version` that is not the tag without the `v` |
| The parts | A part that is not `bootloader`, `partition-table`, `otadata` or `app`, or that is not in the manifest one time. A `file` that is not the name of one of the four images: a path, `..` or another file. An `offset` that is not a number. A `size` or a `sha256` that does not agree with the file |

The script has these options:

| Option | Function |
|---|---|
| `--list <file>` | The JSON of `gh release list --json tagName,isDraft,isPrerelease,publishedAt`. Always necessary |
| `--tags` | The script prints the tags of the releases that it keeps, one on each line, and stops |
| `--releases <dir>` | The folder with one folder `<tag>` for each kept release, as `gh release download "<tag>" --dir <dir>/<tag>` writes it |
| `--out <dir>` | The output folder, for example `site/firmware`. It must be empty, or hold only the files of an earlier run. The script deletes these files first |

[`idf/tools/release/test_site_releases.py`](https://github.com/tmijail/mqtt-rs485-gateway/blob/main/idf/tools/release/test_site_releases.py) is the self-test of the script. It makes fake releases with `make_manifest.py`, and it examines the copy, `index.json` and each refusal. The CI job `host-tests` runs it.

### When the site gets a new release

The workflow [`.github/workflows/docs.yml`](https://github.com/tmijail/mqtt-rs485-gateway/blob/main/.github/workflows/docs.yml) builds the site. After the Zensical build, a step downloads the releases with `gh release download` and runs the script. The output goes into `site/firmware/`. The step needs only the permission `contents: read`.

| Event | The copy | Publication |
|---|---|---|
| Push to `main` | Yes | Yes |
| Push to another branch, pull request, run on request | Yes. The script examines the real releases | No |
| End of a run of the workflow `Release` (`workflow_run`) | Yes, only for a run for a tag that ended with success | Yes, from `main` |

- The workflow `Release` makes the release with `GITHUB_TOKEN`. Such a release starts no other workflow. Thus `docs.yml` starts at the end of each run of `Release`.
- A run on `workflow_run` builds the site from `main`, not from the tag. The release files are only data that the script examines.
- The runs for `main` do not overlap. A new run waits for the end of the run in progress, and no publication stops in the middle.
- Only `docs.yml` publishes the site. Each publication replaces the full site, so two workflows that publish would delete the files of the other workflow.

### Limits of the copy

- The step gets at most 100 releases from `gh release list`. With more releases, the oldest releases are not on the site.
- GitHub Pages has limits for the size of a site and for the traffic of a month. Refer to [GitHub Pages limits](https://docs.github.com/en/pages/getting-started-with-github-pages/github-pages-limits). Each release adds its images to the site. The script has no option yet to keep only the newest releases.
- A damaged release on GitHub stops each build of the site, also the build of a pull request. The builds stop until a maintainer corrects the files of the release.
- The checks find a damaged file or a file of another release. They do not prove the origin of a file, because the images have no signature.

## Limits

- The install tool in the browser reads `index.json` and `manifest.json`, and flashes the parts with esptool-js. Refer to [Flash the firmware from the browser](../how-to/flash-the-firmware-from-the-browser.md). Nobody flashed a board with it (#18).
- The images have no signature. The project has no Secure Boot (#133).
