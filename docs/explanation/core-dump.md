# The core dump

This document tells how the gateway keeps the core dump of a crash and gives it without a USB cable, and why the design is so (REL-1, issue #132). For the procedure, refer to [Get and decode a core dump](../how-to/decode-a-core-dump.md).

## Where the dump goes

A panic, or the task watchdog, saves an ELF dump in the partition `coredump` (64 KB). ELF is the only format of ESP-IDF v6.1. The dump stays in flash until you erase it, also after more restarts.

With a USB cable, `idf.py coredump-info` or `idf.py coredump-debug` reads the dump. A gateway in a panel often has no cable. Thus the running firmware also gives the dump through three channels:

| Channel | What it gives | Reference |
|---|---|---|
| Sparkplug | The NBIRTH metric `Node Info/Core Dump` (Boolean, alias 57) is `true` while a dump is in flash. After an erase, an NDATA with only alias 57 sends `false`. The SCADA can thus tell the operator that a crash occurred | [Sparkplug messages and metrics](../reference/sparkplug-messages-and-metrics.md) |
| REST API | `GET /api/coredump` downloads the raw dump, `GET /api/coredump/summary` gives the summary, `DELETE /api/coredump` erases the dump. `/api/status` has `core_dump`. The card **Gateway** of the Status tab shows **Download** and **Erase** while a dump is in flash | [The REST API](../reference/rest-api.md#core-dump) |
| Shell | `coredump` gives the summary, `coredump dump` prints the dump in base64 between two marker lines, with its size, number of lines and SHA-256 (#293), `coredump erase` erases it (a write command, `CONFIG_GW_SHELL_WRITE_COMMANDS`) | — |

The shell lets a technician with a laptop at the panel get the dump without the network.

Keep the ELF of each firmware that you install. Without the ELF of the same build, nobody can decode the dump.

## The state and the summary

`gw_coredump_init()` reads the state at boot, before the other tasks start. The state is: is a dump present, its size, and is its checksum correct (`esp_core_dump_image_check`, one read of the dump). After that, only an erase changes the state.

The summary comes from `esp_core_dump_get_summary` and `esp_core_dump_get_panic_reason`. It gives the task, the PC, the backtrace, the exception cause and address, and the panic reason. To make the summary, the firmware maps the partition. Thus only a request for the summary reads the partition, not `/api/status`. QEMU does not model the cache freeze of this map. Thus the QEMU build parks core 1 during the summary, as during an update (refer to [The E2E test bench](../reference/e2e-harness.md#emulator-problems-and-their-workarounds)).

The summary also gives the reset reason of this boot. The dump can be older than this boot. The summary of a corrupt dump has the state only.

## The copy at the console

The console of the board is the USB-Serial-JTAG peripheral of the chip. In ESP-IDF v6.1, the standard output (`stdout`) of this console can lose text in two ways:

- The console driver writes nothing while the connection monitor of ESP-IDF reports that no computer is connected. The monitor can report a short disconnection while the cable stays connected. `stdout` writes each line separately, so a complete line is lost.
- The driver has a TX buffer of 256 bytes. When the buffer is full, the driver waits at most 50 ms for the computer. Then it drops the output until the computer reads again. Thus the console cannot stop the gateway when no program reads it.

On the first board, `coredump dump` lost groups of complete lines (#293). The copy still had the END marker, so it looked complete. The download through the REST API had no error.

Thus `coredump dump` has two protections:

- **The copy shows its integrity.** The first line gives the size of the dump, the number of base64 lines and the SHA-256 of the dump. The END marker gives the number of lines again. A reader compares these values with the copy, and thus finds a missing line or a changed line. The base64 lines did not change, so `esp-coredump --core-format b64` decodes them as before.
- **The copy does not use `stdout`.** The command puts the copy directly into the TX buffer of the driver. This path has no connection check and drops nothing. When the buffer is full, the command waits for the computer. If the computer does not read for 3 s, or the copy takes more than 120 s, the command stops. It then writes `-----CORE DUMP INCOMPLETE: …-----` in place of the END marker.

The command reads the dump two times. The first read gives the SHA-256 for the first line, and the second read gives the lines. If the bytes of the two reads are not the same, the copy stops without the END marker. This occurs only when an erase occurs during the copy.

The QEMU build has the console on a UART, which never drops output. There the command uses `stdout`. The E2E examines the format of the copy in QEMU. Only the board can examine the path through the USB driver (refer to [The E2E test bench](../reference/e2e-harness.md#what-qemu-does-not-cover)).

Other long answers of the shell, for example `config show` and `help`, have no such protections. Refer to [Known limitations](../reference/known-limitations.md#the-shell-on-the-usb-console-293).

## Memory

Nothing holds a buffer of the size of the dump (up to 64 KB):

- The download sends pieces of 2 KB.
- `coredump dump` reads pieces of 912 B (16 lines of 57 bytes). It reads the dump two times (refer to [The copy at the console](#the-copy-at-the-console)).
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

The E2E examines the core dump in QEMU with the shell command `panic`, which exists only in the emulator build. The checks are in [The E2E scenarios](../reference/e2e-scenarios.md).
