# Why the firmware has this architecture

This document gives the reasons for the structure of the firmware: the logic, the ownership, the MQTT session and the memory. For the data (the components, the tasks, the sizes and the thresholds), refer to [The architecture of the firmware](../reference/architecture.md).

## Pure C logic with no IDF

All the logic that can have an error is in `gw_core`, in pure C with no IDF. The host tests examine it on a computer, also with AddressSanitizer and UndefinedBehaviorSanitizer. The other components only connect `gw_core` to ESP-IDF. Thus most of the firmware has tests, also without a board. Put new logic in `gw_core`, with a host test.

The board data is in one declarative file, `gw_bsp.c`. The drivers contain no board data. Some board data comes only from indirect evidence (refer to [Board data to verify](../reference/board-data-to-verify.md)). Thus a correction after a measurement changes only one file, or one option in `menuconfig`.

## One owner for each resource

- **The RS485 bus has one owner (MB-16).** Only the `rs485_bus` task uses the UART. The DCMD writes, the Modbus TCP requests and the test reads go to it through a queue. Thus two transactions never overlap on the half-duplex bus, and the timing of t3.5 stays correct.
- **The expander has one writer.** Only the `local_io` task writes the TCA9554. The NCMDs, the web UI, the local rules, the pulse DOs and the safe state all go through it. Thus two sources never write the DOs at the same time.
- **The bus and the network are on different cores.** The RS485 bus runs on core 1. All the network stack runs on core 0. Thus a load on the network does not change the timing of the bus.

## Producers do not block consumers

A device update or a DI change sets an atomic flag, and can wake the `sparkplug` task. When the wake is lost, no state is lost, because the task reconciles all the flags at each loop. Thus a slow broker never stops the polling.

Data that must not be lost (the commands, the write confirmations and the DCMD writes) goes through bounded static queues. A bounded queue gives a known memory use and a clear error when it is full.

## A lock for cJSON (#106)

The hooks of cJSON (the allocator), its error pointer and the out-of-memory flag of `gw_config` are global to the process. A number of tasks parse JSON: the web UI, the shell and the boot. Without a lock, two operations at the same time can cause a false "out of memory" error, or hide a real one. An example is a URL upload through the web UI during a shell `config load`. The lock is recursive, because an operation can call a second operation that also holds it.

## The rules of the MQTT session

- **One bdSeq for each CONNECT packet.** The Sparkplug specification requires that bdSeq increases by 1 for each CONNECT (`tck-id-topics-nbirth-bdseq-increment`). The broker can publish the Will of an earlier attempt later. Thus an attempt that can have sent its CONNECT packet keeps its bdSeq, and a newer NBIRTH never has the same bdSeq.
- **The backoff resets only after a stable session.** Some brokers accept the connection and cut it at once (an ACL, a duplicate `client_id`, a connection limit). A reset at the CONNACK would cause attempts at the minimum interval without end. With the reset after one keepalive, the attempts increase to the maximum interval.
- **The births wait for the SUBACK.** A node that the broker shows as online must receive its commands. When the subscription fails, the task disconnects before the NBIRTH, so the broker discards the Will.
- **No session before a valid clock (#160, decision of the product owner).** The NBIRTH and the data must have valid timestamps. Thus the task waits for the clock, and the first NBIRTH has valid timestamps.
- **A new session on a new interface.** A TCP session cannot move to a different interface. The task ends the old session with an NDEATH, and starts a new session with a new bdSeq and all the births.
- **Failback with hysteresis.** A cable that connects and disconnects many times does not cut the session at each bounce.

## The memory design

### The firmware assumes the PSRAM (#135)

The board has 8 MB of PSRAM. The product owner decided that the firmware does not support a board without PSRAM (#135). Thus the firmware needs no smaller sizes and no second memory budget.

All the own memory goes to the PSRAM. When the PSRAM is full, the allocation fails, and the functions do not use internal RAM in its place. The internal RAM stays for Wi-Fi, lwIP, esp-mqtt, the task stacks and DMA, which cannot use the PSRAM.

### The allocation at boot, not `EXT_RAM_BSS_ATTR`

To put `.bss` in PSRAM, IDF requires `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`. IDF does not permit this option with `CONFIG_SPIRAM_IGNORE_NOTFOUND`. Thus a board with a PSRAM that IDF does not detect would stop in the boot, and it could not report the fault. With the allocation at boot, the fault mode can run from internal RAM, and the operator can see the cause.

### The thresholds of the internal RAM (#80)

At the worst case, the internal RAM must hold three items: an MQTT command with its TLS record, the lwIP buffers and one TLS record of 16 KB. This gives the thresholds of the E2E: 8 KB minimum free, and a free block of 17 KB at the validation. The internal RAM is shared, so these thresholds must never go down.

### Dynamic TLS buffers (#80)

With fixed buffers, each TLS session keeps 16 KB of input. MQTT over TLS and two HTTPS sessions then fragmented the internal RAM: 92 KB were free, but no contiguous block of 32 KB. With `CONFIG_MBEDTLS_DYNAMIC_BUFFER`, a session holds its record buffers only while a record is in progress.

### Small changes can be important

The heap of IDF (TLSF) rounds each allocation up to its size class. Thus some hundred bytes can move a structure into the next class. FW-1 obeys these rules, so that it uses no memory when no update is in progress:

- It creates its timers only when necessary (an image on trial).
- It allocates the buffers of a transfer at the start, and releases them at the end.
- It formats its state in place, under a mutex, with no copy buffers.
- It puts its state data (approximately 590 B) in the RTC FAST memory. This memory has 8 KB, and the heap almost does not use it.

The engine `gw_node_t` must stay in the 21 504 B class. A `_Static_assert` in `gw_edge.c` makes sure of it. Where two buffers are never in use at the same time, they share their memory: for example `vals` (the values of one device) and `hist_str` (the strings of one historical message).

### One block for the print of the document (#91)

`cJSON_Print` made its buffer two times larger at each step, and copied it at each move. A document of 12 KB then held up to 32 KB of heap. `gw_cfgdoc` now writes the text in two passes: it counts the size, allocates one block of that size, and writes. The text is the same, byte for byte (a host test compares it with `cJSON_Print`).

### No allocation in the polling and publish path

An allocation can fail or fragment the heap at any time. Thus the path that runs all the time (the polling, the encoding and the publish) uses only memory that the firmware allocated at boot. The spill keeps its files open, so that it allocates only when it opens a file.
