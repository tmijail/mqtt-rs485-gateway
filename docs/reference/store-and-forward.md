# Store & forward

This document gives the data of store & forward: the RAM buffer, the microSD card, the retention and the duplicates after a restart.

- For the keys, refer to [`sparkplug.store_forward`](configuration.md#sparkplugstore_forward).
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

## The microSD card

With `sd.enabled`, the oldest samples go from the RAM buffer to the card in blocks of 4 KB when the buffer is three quarters full. When the session starts again, the gateway publishes the samples of the card first.

> **Caution: Read the GPIO45 warning in [`idf/README.md` §6.5](../../idf/README.md#65-slow-broker-and-command-limits) before you set `sd.enabled`.** GPIO45 (D0 of the card) is a strapping pin. A pull-up on D0 can stop the boot of the board.

### The card and its format

- The card is only for the gateway.
- The web UI ("microSD card", "Format the card") and the shell command `card format confirm` format the card. The format makes one MBR partition with FAT32 and clusters of 32 KB (FAT16 below 2 GB).
- The format deletes all the data on the card. The gateway never formats a card by itself: the web UI asks you to type `FORMAT`, and the shell needs the word `confirm`.
- The format runs in the background, and can take some minutes. The spill stops first, and the RAM buffer continues. After the format, restart the gateway to use the card.
- The format operates also on a card that does not mount, and also when `sd.enabled` is `false`.
- The QEMU build without PSRAM refuses the format.

| Card | Result |
|---|---|
| Formatted by the gateway | Mounts. The gateway uses all of `max_kb` |
| FAT with clusters of 4 KB, formatted by a PC | Mounts. The gateway uses a maximum of 512 MB |
| No file system, exFAT, NTFS, ext4, or a GPT partition table | Does not mount. Most cards of 64 GB or more have exFAT when you buy them |

The status page and `GET /api/status` (`card`) tell why a card does not mount. Use an industrial card (pSLC) if possible. The gateway writes each block one time, with no FAT or directory write in normal operation.

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
| A change of the datatype of a tag (from `type`, `scale` and `offset`) | `area`, `address`, `bit`, `word_order`, `deadband`, `writable`, `units`, `eng_low`, `eng_high` |
| A change of the device `type`, `group_id`, `edge_node_id`, `device_prefix` or `naming` | A new `scale` or `offset` with the same datatype |
| A change of the `length` of a `bytes` tag | The `length` of a `string` tag, and all the other sections |

A firmware update keeps the data.

### Planned restarts

Before a planned restart, the gateway moves the samples of the RAM buffer to the card. After the boot, it publishes these samples one time, in order, before the newer data.

- These restarts are planned: the shell `restart`, `POST /api/restart`, the NCMD `Node Control/Reboot`, the restart after a firmware update, and the restart after a saved configuration.
- The move has a time limit of 5 s. The log tells how many samples moved and how many are lost.
- The gateway does not move the RAM buffer in these cases: after a factory reset, during a format, when the spill is off or failed, and before the boot read the card.
- The gateway also does not move it when a saved configuration changes the hash. The log tells why.
- An unplanned restart (a power cut, a watchdog, a crash) loses the RAM buffer. Only the data on the card stays.

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
