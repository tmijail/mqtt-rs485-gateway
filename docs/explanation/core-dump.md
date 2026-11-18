# The core dump

This document tells how the gateway keeps the core dump of a crash and gives it without a USB cable, and why the design is so (REL-1, issue #132). For the procedure, refer to [Get and decode a core dump](../how-to/decode-a-core-dump.md).

> **Warning: Do not use this document as verified data for the board.** The core dump ran only in QEMU. Nobody tested it on the board (#18).

## Where the dump goes

A panic, or the task watchdog, saves an ELF dump in the partition `coredump` (64 KB). Since ESP-IDF 6.0, ELF is the only format: the option `CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF` does not exist any more. The dump stays in flash until you erase it, also after more restarts.

With a USB cable, `idf.py coredump-info` or `idf.py coredump-debug` reads the dump. A gateway in a panel often has no cable. Thus the running firmware also gives the dump through three channels:

| Channel | What it gives | Reference |
|---|---|---|
| Sparkplug | The NBIRTH metric `Node Info/Core Dump` (Boolean, alias 57) is `true` while a dump is in flash. After an erase, an NDATA with only alias 57 sends `false`. The SCADA can thus tell the operator that a crash occurred | [Sparkplug messages and metrics](../reference/sparkplug-messages-and-metrics.md) |
| REST API | `GET /api/coredump` downloads the raw dump, `GET /api/coredump/summary` gives the summary, `DELETE /api/coredump` erases the dump. `/api/status` has `core_dump`. The Status tab shows **Download** and **Erase** while a dump is in flash | [The REST API](../reference/rest-api.md#core-dump) |
| Shell | `coredump` gives the summary, `coredump dump` prints the dump in base64 between two marker lines, `coredump erase` erases it (a write command, `CONFIG_GW_SHELL_WRITE_COMMANDS`) | — |

The shell lets a technician with a laptop at the panel get the dump without the network.

Keep the ELF of each firmware that you install. Without the ELF of the same build, nobody can decode the dump.

## The state and the summary

`gw_coredump_init()` reads the state at boot, before the other tasks start. The state is: is a dump present, its size, and is its checksum correct (`esp_core_dump_image_check`, one read of the dump). After that, only an erase changes the state.

The summary comes from `esp_core_dump_get_summary` and `esp_core_dump_get_panic_reason`. It gives the task, the PC, the backtrace, the exception cause and address, and the panic reason. To make the summary, the firmware maps the partition. Thus only a request for the summary reads the partition, not `/api/status`.

The summary also gives the reset reason of this boot. The dump can be older than this boot. The summary of a corrupt dump has the state only.

## Memory

Nothing holds a buffer of the size of the dump (up to 64 KB):

- The download sends pieces of 2 KB.
- `coredump dump` reads pieces of 912 B (16 lines of 57 bytes).
- The request allocates these buffers and the summary (approximately 300 B) from the heap, in PSRAM. It releases them at the end.
- The state uses no allocation.

## Security

The dump is a copy of the RAM at the crash. It can contain secrets in clear text: the MQTT password, TLS session keys and the private key of the device certificate. Thus:

- All `/api/coredump` routes need the web password in all modes, also in recovery mode.
- `DELETE` has the CSRF check of all writes.
- The shell asks for no password. The console needs physical access, and that access also reads all of the flash through USB. Refer to [Features that burn eFuses](no-efuse-features.md#what-this-means-for-the-security-of-a-gateway).

Erase the dump after a download. Keep the file as safe as the passwords of the gateway.

## Concurrency

A mutex serializes the reads, the summary and the erase. If an erase occurs during a download, the next read fails. The download then stops incomplete: the connection closes before the last chunk.

## Verification

The E2E examines the core dump in QEMU with the shell command `panic`, which exists only in the emulator build. The results are in [Tests](../reference/tests.md).
