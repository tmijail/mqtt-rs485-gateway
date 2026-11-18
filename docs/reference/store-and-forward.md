# Store & forward

This document gives the data of store & forward: the RAM buffer, the microSD card, the retention and the duplicates after a restart.

- For the keys and their controls in the web UI (the section **Store & forward** of the card **Sparkplug**), refer to [`sparkplug.store_forward`](configuration.md#sparkplugstore_forward).
- For the messages, refer to [Sparkplug messages and metrics](sparkplug-messages-and-metrics.md).

> **Warning: Do not use this document as verified data for the board.** The firmware has never run on the board (#1, #18). The SDMMC driver, the FAT file system and the format of the card are compile-only. The host tests and the E2E in QEMU examine the buffer and the ring with a simulated card.

## The RAM buffer

| Item | Value |
|---|---|
| Size | 40 000 samples in PSRAM (960 KB, Kconfig `GW_SF_SAMPLES`) |
| One sample | One change of one metric: 24 B. A text uses 1 + length / 8 samples |
| Full buffer | The gateway writes over the oldest samples |
| Exact values | The buffer keeps the 64-bit integers and the texts exactly |

- The gateway keeps each change that it would publish, with the same report by exception and deadband rules.
- "Without a session" includes a broker that is not available, and a primary host that is offline. It starts from the first NBIRTH of the boot.
- Without a valid clock, the gateway keeps nothing and sends no history.
- When the session starts again, a device can be offline and have no DBIRTH. The gateway discards the samples of that device.
- The buffer does not keep the DBIRTH, the DDEATH or the answers to commands. The DBIRTH of the next session gives the state.
- The buffer keeps nothing between the boot and the first session. The first DBIRTH has the current value.
- The gateway publishes 4 historical messages in each pass of the `sparkplug` task, between the live data. The hosts know them from `is_historical`.
- The shell and the log count the discarded samples and the overwritten samples.
- 40 000 samples are approximately 11 h of one tag that changes each second. They are approximately 2 min of 256 tags that change in each cycle.
- During a session, the gateway does not use the buffer. On a slow broker, it publishes the last value of each device in each pass.

## The microSD card

With `sd.enabled`, the oldest samples go from the RAM buffer to the card in blocks of 4 KB when the buffer is three quarters full. When the session starts again, the gateway publishes the samples of the card first.

> **Caution: Read [The microSD card and GPIO45](#the-microsd-card-and-gpio45) before you set `sd.enabled`.** GPIO45 (D0 of the card) is a strapping pin. A pull-up on D0 can stop the boot of the board.

### The card and its format

- The card is only for the gateway (decision of the product owner, #138). Do not put other files on the card. Format it with the gateway before the first use.
- The web UI ("microSD card", "Format the card") and the shell command `card format confirm` format the card. The format makes one MBR partition with FAT32 and clusters of 32 KB (FAT16 below 2 GB).
- The format deletes all the data on the card. The gateway never formats a card by itself: the web UI asks you to type `FORMAT`, and the shell needs the word `confirm`.
- The confirmation of the web UI gives the card (name, size and state) and the number of files and folders that are not from the gateway. Its red button "Format the card" stays disabled until the field has `FORMAT`. Esc and "Cancel" close it, and the card does not change.

  ![The confirmation 'Format the microSD card?': the card SD64G, 59.5 GB, cannot mount. The format erases all the contents of the card, also its 3 files or folders that are not from the gateway. The field has FORMAT, and the red button 'Format the card' is enabled.](../images/card-format-dialog.png)

- The format runs in the background, and can take some minutes. The spill stops first, and the RAM buffer continues. After the format, restart the gateway to use the card.
- The format operates also on a card that does not mount, and also when `sd.enabled` is `false`.
- The format and the spill need the PSRAM (decision of the product owner, #135). The format task uses internal RAM only while it runs.
- Scripts can use `POST /api/card/format?confirm=1`, with the Basic auth and the CSRF check of the other writes.
- One format runs at a time. A second request gets `409` (busy).
- The format writes zeros on the two GPT headers, then a new MBR with one FAT partition. The data area starts at a multiple of 4 MB, the allocation unit of SDHC and SDXC cards.
- Last, the format task mounts the new volume one time to check it. The shell `status` and `card`, and `/api/status` (`card_format`), give `running`, `done` or `failed` with the reason.

| Refusal | Answer |
|---|---|
| The QEMU build: there is no card in the build | `501`, reason `no_support`. Nothing changes |
| No PSRAM (the fault mode, #135) | `501`, reason `no_psram` |
| A card that does not answer | The format ends as `failed` (`no card answers (…)`) |

| Card | Result |
|---|---|
| Formatted by the gateway | Mounts. The gateway uses all of `max_kb` |
| FAT with clusters of 4 KB, formatted by a PC | Mounts. The gateway uses a maximum of 512 MB |
| No file system, exFAT, NTFS, ext4, or a GPT partition table | Does not mount. Most cards of 64 GB or more have exFAT when you buy them |

ESP-IDF v6.1 sets `FF_FS_EXFAT` to 0 and `FF_LBA64` to 0. Thus FatFs reads no exFAT and no GPT. The boot never formats a card (no `format_if_mount_failed`).

The status page, the shell (`sdcard   NOT MOUNTED: …`) and `GET /api/status` (`card.state`, `card.detail`) tell why a card does not mount. Use an industrial card (pSLC) if possible. The gateway writes each block one time, with no FAT or directory write in normal operation.

### The files

| File | Contents |
|---|---|
| `/sf/r0000.sf`, `/sf/r0001.sf`, … | The ring of blocks |
| `/sf/ringa.sf`, `/sf/ringb.sf` | The two copies of the superblock |

A PC does not need to read these files. The gateway deletes the files of the earlier card format (`NNNNNNNN.sf`) at the boot, and does not publish their data.

### When the gateway starts a new ring

The data on the card stays after a restart and after a power cut. The ring keeps a hash of the configuration fields that identify a sample. When this hash changes, the gateway starts a new ring. It does not publish the old data.

| These changes start a new ring | These changes keep the data |
|---|---|
| A device or a tag that you add, remove, rename or move | `poll_ms`, `slow_poll_ms`, `scan`, `unit_id`, `offline_after`, `block_reads`, `max_gap` |
| A change of the datatype of a tag (from `type`, `scale` and `offset`) | `area`, `address`, `bit_offset`, `bit_width`, `word_order`, `deadband`, `writable`, `units`, `eng_low`, `eng_high` |
| A change of the device `type`, `group_id`, `edge_node_id`, `device_prefix` or `naming` | A new `scale` or `offset` with the same datatype |
| A change of the `length` of a `bytes` tag | The `length` of a `string` tag, and all the other sections |
| | `write_multiple`, `enabled`, `write_min`, `write_max` |

A firmware update keeps the data. A disabled device has no DBIRTH, so the replay discards its samples. The samples of the DIs, the DOs and the counters do not depend on the configuration.

For the fields of the hash, refer to [The configuration hash](#the-configuration-hash).

### Planned restarts

Before a planned restart, the gateway moves the samples of the RAM buffer to the card. After the boot, it publishes these samples one time, in order, before the newer data.

- These restarts are planned: the shell `restart`, `POST /api/restart`, the NCMD `Node Control/Reboot`, the restart after a firmware update, and the restart after a saved configuration.
- The move has a time limit of 5 s. The log tells how many samples moved and how many are lost.
- The gateway does not move the RAM buffer in these cases: after a factory reset, during a format, when the spill is off or failed, and before the boot read the card.
- The gateway also does not move it when a saved configuration changes the hash (#141). For example, a new tag name changes the hash, but a new broker address, `deadband` or `poll_ms` does not. The log tells why.
- The gateway also does not move it at the rollback of a new image on trial (FW-1). The rollback does not depend on the tasks of the bad image.
- An unplanned restart (a power cut, a watchdog, a crash, a brownout) loses the RAM buffer. Only the data on the card stays.
- A card failure during the move stops the move and the spill at once. The log tells the samples that moved and the samples that are lost.
- The `sparkplug` task owns the card and does the move. The web server, the shell and the firmware update only set a request.
- The `sparkplug` task must start the restart in 20 s (`RESTART_ANSWER_MS`). If not, the `esp_timer` task restarts the gateway without the move and without the NDEATH.

The time limit of 5 s is `FLUSH_LIMIT_MS` in `gw_edge.c`. The move is at most three quarters of the RAM buffer: approximately 180 blocks. At 1 to 5 ms for each block write, this takes less than 1 s. The task feeds the watchdog after each card operation, so the limit is for the operator who waits for the restart.

## The microSD card and GPIO45

> **Warning: Examine the schematic of the board before you set `sd.enabled`.** D0 of the card must have no pull-up. The card must not keep D0 high at reset. If GPIO45 is high at reset, the 3.3 V flash of the module can fail to boot.

- The card uses SDMMC in 1-bit mode: CLK GPIO48, CMD GPIO47, D0 GPIO45.
- GPIO45 is the strapping pin of VDD_SPI. If it is high at reset, the ROM supplies 1.8 V to the flash.
- Nobody verified the circuit of D0 on the schematic or on a board.
- For this reason, the spill is off by default. When you enable it, the boot log gives a warning.

## Retention

The gateway keeps only the changes: a tag goes into the buffer only when it changed by more than its `deadband`. One block of 4 KB holds 169 samples. Thus 1 MB of `max_kb` holds 43 264 samples.

hours ≈ `max_kb` × 0.01174 / (changes per second)

| `max_kb` | Changes per second | Retention |
|---|---|---|
| 131072 (128 MB) | 50 | Approximately 31 h |
| 131072 (128 MB) | 5 | Approximately 13 days |

The RAM buffer adds 40 000 samples. The `deadband` is the main control: set it a little above the noise of the sensor.

## Duplicates after a restart

The gateway saves its replay position on the card after each 64 KB of published history (2 730 samples).

| Event during a replay | Maximum of duplicate samples |
|---|---|
| A restart or a power cut | 64 KB (approximately 2 700 samples) |
| A planned restart | None: the gateway saves the position first |
| A power cut that damages the save | 128 KB |
| Both copies of the superblock damaged | All the data on the card |

- The duplicates have the same timestamps and values as the first copy. The host or the historian must remove them, for example with one value for each tag and timestamp.
- The history uses QoS 0. A power cut can also lose the messages that the TCP connection did not send yet.

## The ring on the card

This section is for developers. The policy is in `gw_sfspill` (`gw_core`), with the host tests `test_sf`, `test_sfcfg` and `test_cardfmt`.

### Extents, blocks and superblocks

| Item | Value |
|---|---|
| Extent | One file `r0000.sf`, `r0001.sf`, …: min(4 MB, 128 clusters). The gateway makes it at its first use (`lseek` and one `fsync`), and never changes its size |
| Extents in a ring | 1 024 at most: 4 GB with clusters of 32 KB, 512 MB with clusters of 4 KB. This keeps a directory search to 64 sector reads at most |
| `max_kb` | 64 KB to 4 GB. 128 MB by default. When the extent limit makes `max_kb` smaller, the boot log tells you to format the card with clusters of 32 KB |
| Block | 4 KB: a header of 32 B and 169 samples at most. The header has a magic, a version, the sample size, the nonce of the ring, the configuration hash, the block number `seq`, the sample count, flags and a CRC-32C |
| Superblock | Two copies of 64 B, `ringa.sf` and `ringb.sf`. Each copy has a generation, the geometry, the nonce, the hash, the replay position (`tail`), a hint for the next block (`head`) and a CRC |

- A string sample never goes into two blocks.
- The gateway writes a block one time, whole, at a position aligned to 4 KB, with no `fsync`. There is no FAT or directory write in normal operation.
- The replay uses a block only if three conditions are true: the CRC is correct, the nonce is the nonce of the ring, and `seq` modulo the ring size is the position. Thus the gateway never publishes a damaged block, a block of an earlier ring, or a block of an earlier lap.
- The gateway writes the superblock copy with the lower generation. It never writes the two copies less than 500 ms apart.
- It writes a superblock before a block with a new nonce or size. It also writes one after each 32 blocks of spill, and after each 64 KB of replay.
- Only the `sparkplug` loop writes to the card, one block in each pass, when the RAM buffer is three quarters full. The producers never write to the card.
- When the ring is full, the next block replaces the oldest block. The gateway counts its samples as `overwritten unsent`.
- If the card is full before the gateway made all the extents, the ring keeps the extents that it has. The boot log tells this.

### The boot recovery

- The boot only mounts the card. The `sparkplug` loop reads the card in steps after the network starts, or 10 s after the boot. The replay starts only after the recovery.
- The recovery reads the two superblocks and lists the folder. Then it reads forward from the head hint while the blocks follow.
- It skips 64 invalid blocks (256 KB) at most as holes. Reason: a power cut can damage NAND pages of 16 KB that the card programmed before the cut.
- A block after a hole must have the expected `seq`, so a larger skip cannot accept wrong data. The replay counts each hole as lost, and never publishes it.
- When both superblocks are damaged, the gateway reads the first block of each extent. It takes the nonce of most valid blocks, then finds the head with a scan.
- After this vote, the replay position is lost. The gateway can send all the data on the card again, but never damaged data.
- A new configuration hash or a new `max_kb` gives a new superblock with a new nonce. The old blocks are then not valid, and the gateway deletes no file for them.
- When the ring becomes smaller, the gateway deletes the extra extents, one in each pass.

The log line `store & forward: card ready …` and the shell `spill` line give the result of the recovery:

| Result | Meaning |
|---|---|
| `new ring: no valid ring on the card` | No valid superblock and no valid block |
| `new ring: another configuration or geometry` | A valid superblock with another configuration hash, `max_kb` or cluster size |
| `ring found with its superblock` | The normal result after a restart |
| `ring found again without a superblock (vote)` | Both superblocks were damaged, and the vote found the ring. The gateway publishes its data again |

### The configuration hash

A sample on the card has a device index, a tag index and a value. The replay makes a metric from them with the configuration of the current boot. Thus the ring keeps a hash of the fields that set their meaning.

- The fields in the hash: `sparkplug.group_id`, `edge_node_id`, `device_prefix` and `naming`. For each device, in order: `name`, `type` and its range of tags. For each tag, in order: `name`, the Sparkplug datatype and, for a `bytes` tag, `length`.
- `gw_sfspill_cfg_hash()` (`gw_sfcfg.c`) calculates the hash from this explicit list. Each field has a fixed size and byte order. The input starts with a format number.
- The hash never uses the bytes of a C structure. Thus a firmware update that changes `gw_tag_cfg_t` or `gw_device_cfg_t` keeps the data on the card.
- A new field in `gw_tag_cfg_t` or `gw_device_cfg_t` stops the build. Put the field in the hash or in the list of excluded fields next to it. The host test `sfcfg_guard` makes sure that the guard operates.
- `test_sfcfg` has a golden value for a fixed configuration. `test_sfcfg_packed` gets the same value with packed structures and enums of 1 byte.

### Files on the card

- The gateway never touches a file in `/sf` with another name, or with a number at or above `0x80000000` (#134).
- The gateway deletes the files of the earlier card format (`NNNNNNNN.sf`) at the boot, with no migration. It truncates each file in steps of 128 clusters at most, then removes it.
- The gateway never renames a file. Reason: a power cut during a FAT rename can link two files to the same clusters.

### Failures

- An I/O failure stops the spill until the next boot (#134). The RAM buffer then overwrites its oldest samples again.
- The shell shows the samples on the card apart: `spill    FAILED (…): N samples stay on the card unsent`. The next boot finds them again.
- When FatFs closes an extent, it writes its directory entry again. A power cut during that write can damage the directory sector. Only the board tests of #131 can show the effect.
- On an almost full or fragmented card, the first allocation of an extent can read a large part of the FAT.

### The values for the board tests (#131)

- The shell `status` gives a `card` line: the cluster size and, for each card operation, the count, the average time and the maximum time. It also gives the errors, and the longest time without a watchdog feed of the `sparkplug` task.
- The `spill` line gives the samples on the card and the lost samples. It also gives the blocks, the superblock writes, the checkpoints, the result of the boot recovery and the deletes.
- The shell command `card` gives the `card`, `sdcard` and `format` lines.

These tests on the board remain (#131):

- the format from the web UI, and the spill after the restart. Use a card with no file system, an exFAT card of 64 GB or more, an NTFS card and a GPT card;
- the duration of the format on cards of 32, 64 and 128 GB, and the free stack of the task `card_format`;
- the time of each card operation on 3 card classes, with clusters of 32 KB and of 4 KB;
- a broker outage of 24 h;
- at least 200 power cuts for each card class, with a counter tag, and `fsck.vfat` on a PC at intervals.

### QEMU

QEMU has no SDMMC. The mount, SDMMC, FAT and the format are compile-only.

- The E2E uses a LittleFS directory in place of the card (`CONFIG_GW_SF_SPILL_LITTLEFS`), with a "cluster" of 32 B. LittleFS needs an `fsync` after each write; the card does not.
- The QEMU build with PSRAM has a RAM buffer of 1 000 samples (`sdkconfig.qemu-psram`), so that a short outage spills.
- The emulator-only shell command `card format simulate <ms>` runs the format task with no write.
- The QEMU build without PSRAM has a RAM buffer of 512 samples in internal RAM, and no spill. The shell gives `spill    off: it needs PSRAM, and none was detected`.
