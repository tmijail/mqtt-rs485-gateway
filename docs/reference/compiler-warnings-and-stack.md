# Compiler warnings and the stack data

This document gives the compiler options of the own components, the limit of the stack frame, and the stack report of the tasks (issue #227). For the command that makes the report, refer to [Build and flash the firmware](../how-to/build-the-firmware.md#make-the-stack-report).

## The compiler options

[`cmake/gw_warnings.cmake`](../../idf/cmake/gw_warnings.cmake) holds the compiler options of the own components: `gw_*` and `main`. Each of these components calls `gw_target_options()`. The components of ESP-IDF, the managed components and `components/nanopb` keep their own options.

| Option | Firmware (`gw_*`, `main`) | Host build of `gw_core`, host tests | What it finds |
|---|---|---|---|
| `-Wall -Wextra -Werror` | yes | yes | the usual warnings, as errors |
| `-Wconversion -Wno-sign-conversion` | `gw_core` only | `gw_core` only | an implicit conversion that can change a value |
| `-Wshadow` | yes | yes | a local variable that hides another variable |
| `-Wformat=2` | yes | yes | a format string that is not a literal, and format security |
| `-Wnull-dereference` | yes | yes (no effect at `-O0`) | a path where the optimizer proves a NULL dereference |
| `-Wvla` | yes | yes | an array on the stack with a size that is not a constant |
| `-Wstrict-prototypes` | yes | yes | `void f()` in place of `void f(void)` |
| `-Wcast-align=strict` | yes | yes | a pointer cast to a type with a larger alignment |
| `-Wundef` | no | yes | an undefined macro in `#if` |
| `-fstack-usage -fcallgraph-info=su` | yes | no | the stack data for `tools/stack_report.py` |
| `-Wstack-usage=2048` | yes | no | a function with a stack frame of more than 2 048 B |

- On the Xtensa target, `-Wcast-align=strict` is the same as `-Wcast-align`, because the target needs aligned access. On the host, only `=strict` gives the warning.
- The default host build has no optimization (`-O0`), thus `-Wnull-dereference` finds nothing there. The firmware build (`-Os`) and the host build at `-O2` of CI do the check.
- These options do not change the generated code. They only add diagnostics and the stack data.
- The project does not hide warnings with `#pragma`. Correct the code instead.

## No `-Wundef` in the firmware

The headers of ESP-IDF evaluate undefined macros in `#if`, for example `BOOTLOADER_BUILD` and `CONFIG_FREERTOS_SMP`. With `-Wundef`, the release build gave 3 615 warnings from 67 headers of ESP-IDF.

Also, ESP-IDF does not define a bool option that is off. Thus `#if CONFIG_GW_IO_SIMULATED` is correct code, but `-Wundef` gives a warning for it. The project does not hide warnings with `#pragma`, thus the firmware does not use `-Wundef`.

The host test `check_kconfig_names` finds a misspelled option name instead. It examines each `CONFIG_GW_` name in the sources of `gw_*` and `main`, and in the `sdkconfig` files. It fails when no `Kconfig` file of the project declares the name. This check also finds a misspelled name in `#ifdef`, where `-Wundef` finds nothing.

## The limit of the stack frame: `-Wstack-usage=2048`

**Decision of the product owner (#227):** the limit is 2 048 B. The firmware build of the own components uses `-Wstack-usage=2048`. With `-Werror`, a larger frame stops the build. A frame that the compiler cannot bound (`alloca`) also stops it. The host build has no limit, because the frames on the host are different.

The limit stops a new frame of more than half of the stack of `rs485_bus` (4 096 B).

All own functions have a static frame (no VLA and no `alloca`). These are the largest frames, measured at #227:

- `gw_storage_load_config` 1 664 B (boot, main task);
- `send_why` 1 520 B and `send_error` 1 360 B (`gw_web.c`, task of the web server);
- `load_pem_certs` 1 152 B (`gw_edge.c`);
- `status_card` 1 136 B (`gw_web.c`);
- `decode_metric` 1 104 B (`spb.c`). It decodes a metric of an NCMD or a DCMD in a callback of `pb_decode()`. Thus the stack report cannot put it in a task.

Of 1 150 own functions, 88 have a frame of more than 256 B, 31 of more than 512 B, and 6 of more than 1 024 B. A limit of 1 024 B needs a decision about these 6 functions. A trial with 1 024 B stopped the build at `decode_metric`.

## The stack report

[`tools/stack_report.py`](../../idf/tools/stack_report.py) reads the `.su` and `.ci` files of the own components in a build folder. By default, it reports the tasks `rs485_bus` and `sparkplug`. The option `--task NAME=FILE:FUNCTION` adds a task. The CI job `firmware` puts `stack.txt` in its artifact `firmware-esp32s3`.

### How the script finds the functions of a task

- `-fcallgraph-info=su` writes the call graph of each source file, with the frame of each function. The script joins the graphs of all own source files.
- A function belongs to a task when the graph reaches it from the entry function of the task: `bus_task` in `gw_bus.c`, `edge_task` in `gw_edge.c`.
- A function of `gw_core` that two tasks call is in the list of the two tasks.

The script uses the call graph, not the component or the source file. The reason: `gw_core` has no task of its own. The Modbus master and the poller run in `rs485_bus`, and the Sparkplug engine runs in `sparkplug`. Also, `gw_edge.c` has callbacks of esp-mqtt that run in the task of esp-mqtt, not in `sparkplug`. A list by file puts these functions in the incorrect task.

### The contents of the report

For each task, the report gives:

- the functions of the task with the largest frames;
- the deepest call path through the own code, with the sum of the frames;
- the calls out of the own code (ESP-IDF, newlib, nanopb) and the indirect calls, with the own stack above them.

### The limits of the report

- The script does not know the stack of ESP-IDF, newlib and nanopb (they compile without `-fstack-usage`), or of a call through a function pointer. Thus the deepest path is a lower bound.
- A recursion counts one time, and the report names it.
- The real high-water mark comes from `uxTaskGetStackHighWaterMark()` (`log_stacks()` in `gw_edge.c`) in the E2E.

### ccache

The own components compile without ccache: `RULE_LAUNCH_COMPILE` is empty for their targets. At a hit, ccache 4.9 gives the object and the `.su` file again, but not the `.ci` file. The script fails when a C object file of the own components has no `.ci` file.

### The values at #227

| Task | Stack | Deepest own path | The largest frames |
|---|---|---|---|
| `rs485_bus` | 4 096 B | 832 B: `bus_task` → `serve_pending` → `gw_poller_write_value` → `bit_request` → `execute_n` → `mb_master_execute_retries` → `mb_build_request` → `finish` → `mb_crc16` | `gw_poller_write_value` 304, `gw_poller_poll` 176, `serve_pending` 128, `bit_request` 112, `gw_poller_forward` 112 |
| `sparkplug` | 6 144 B | 1 344 B: `edge_task` → `op_reboot` → `gw_fw_restart_requested` → `restart_confirm_on_core0` → `confirm` → `gw_storage_save_last_good` → `gw_lkg_encode` → `put_u16` → `put` | `gw_storage_save_last_good` 688, `publish_nbirth` 384, `gw_node_io_changed` 336, `publish_io` 336, `confirm` 304 |

- In `sparkplug`, `spb_encode()` calls `pb_encode()` of nanopb with 1 056 B of own stack above it. The stack of nanopb adds to this value.
- In `rs485_bus`, the UART read is an indirect call (`rx()` in `mb_master.c`), with 768 B of own stack above it.

For the current values, read `stack.txt` in the artifact of the last CI run.
