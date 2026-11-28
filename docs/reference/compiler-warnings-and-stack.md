# Compiler warnings and the stack data

This document gives the compiler options of the own components, the limit of the stack frame, and the stack report of the tasks (issue #227). It also gives the build with the GCC static analyzer (issue #230). For the commands, refer to [Make the stack report](../how-to/build-the-firmware.md#make-the-stack-report) and [Run the static analyzer](../how-to/build-the-firmware.md#run-the-static-analyzer).

## The compiler options

[`cmake/gw_warnings.cmake`](https://github.com/tmijail/mqtt-rs485-gateway/blob/main/idf/cmake/gw_warnings.cmake) holds the compiler options of the own components: `gw_*`, `main` and the bootloader component `gw_boot_de` (#300). Each of these components calls `gw_target_options()`. The components of ESP-IDF, the managed components and `components/nanopb` keep their own options.

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
| `-fstack-usage -fcallgraph-info=su` | yes | `-fcallgraph-info=su` for `gw_core` and nanopb only | the stack data for `tools/stack_report.py`. On the host, the call graph for the rule checks of `tools/check_rules.py` (#228) |
| `-Wstack-usage=2048` | yes | no | a function with a stack frame of more than 2 048 B |
| `-fanalyzer` | only with `-D GW_ANALYZER=ON` (CI job `analyzer`) | no | the paths through a function that the analyzer can follow: a NULL dereference, a leak, a use after free, undefined behaviour. Refer to [The static analyzer](#the-static-analyzer) |

- On the Xtensa target, `-Wcast-align=strict` is the same as `-Wcast-align`, because the target needs aligned access. On the host, only `=strict` gives the warning.
- The default host build has no optimization (`-O0`), thus `-Wnull-dereference` finds nothing there. The firmware build (`-Os`) and the host build at `-O2` of CI do the check.
- These options do not change the generated code. They only add diagnostics and the stack data. The release build does not use `-fanalyzer`.
- The project does not hide warnings with `#pragma`. Correct the code instead.

## The static analyzer

The host tests compile only `gw_core`. The IDF adapters (`gw_web`, `gw_fw`, `gw_storage`, `gw_edge`, `gw_net`, `gw_bus`, `main` and the other `gw_*` components) compile only in the firmware build. The CI job `analyzer` builds the firmware with the GCC static analyzer (`-fanalyzer`, GCC 15.2 of ESP-IDF v6.1) on all own components (issue #230).

- The CMake variable `GW_ANALYZER` (default `OFF`) adds `-fanalyzer` in `gw_target_options()`. Thus it applies only to the own components, in the release configuration. The bootloader is a separate CMake project: `idf/CMakeLists.txt` gives `GW_ANALYZER` to it through `EXTRA_CMAKE_ARGS`, so the analyzer also examines `gw_boot_de`.
- The job uses a separate build folder, `build-analyzer`. The release build and the job `firmware` do not change.
- The project does not use `CONFIG_COMPILER_STATIC_ANALYZER` of ESP-IDF. That option adds `-fanalyzer` to all components, also to ESP-IDF and the managed components, and ESP-IDF has known false positives.
- With `-Werror`, a result of the analyzer stops the build. The job then gives each result in `components/gw_*` and `main`. A result in `bootloader_components/gw_boot_de` also stops the build, but the job does not list it separately.
- The analyzer examines only the source files of the own components. A result in a header of ESP-IDF is possible only on a path through the own code, and it also stops the build.
- The code has some changes for false positives of the analyzer, so that the analyzer can follow the code. Each change keeps the behaviour, and has a comment with the name of the check and the reason. The project does not hide a result with `#pragma` or `-Wno-analyzer-*`.

To correct a result, refer to [Run the static analyzer](../how-to/build-the-firmware.md#run-the-static-analyzer).

## No `-Wundef` in the firmware

The headers of ESP-IDF evaluate undefined macros in `#if`, for example `BOOTLOADER_BUILD` and `CONFIG_FREERTOS_SMP`. With `-Wundef`, the release build gives many warnings from the headers of ESP-IDF.

Also, ESP-IDF does not define a bool option that is off. Thus `#if CONFIG_GW_IO_SIMULATED` is correct code, but `-Wundef` gives a warning for it. The project does not hide warnings with `#pragma`, thus the firmware does not use `-Wundef`.

The host test `check_kconfig_names` finds a misspelled option name instead. It examines each `CONFIG_GW_` name in the sources of `gw_*` and `main`, and in the `sdkconfig` files. It fails when no `Kconfig` file of the project declares the name. This check also finds a misspelled name in `#ifdef`, where `-Wundef` finds nothing.

## The limit of the stack frame: `-Wstack-usage=2048`

**Decision of the product owner (#227):** the limit is 2 048 B. The firmware build of the own components uses `-Wstack-usage=2048`. With `-Werror`, a larger frame stops the build. A frame that the compiler cannot bound (`alloca`) also stops it. The host build has no limit, because the frames on the host are different.

The limit stops a new frame of more than half of the stack of `rs485_bus` (4 096 B).

All own functions have a static frame (no VLA and no `alloca`). The stack report gives the largest frames of each task.

## The stack report

[`tools/stack_report.py`](https://github.com/tmijail/mqtt-rs485-gateway/blob/main/idf/tools/stack_report.py) reads the `.su` and `.ci` files of the own components in a build folder. By default, it reports the tasks `rs485_bus` and `sparkplug`. The option `--task NAME=FILE:FUNCTION` adds a task. The CI job `firmware` puts `stack.txt` in its artifact `firmware-esp32s3`.

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

### The values of a build

The documentation does not record the values of the report, because each change of the code changes them. Read `stack.txt` in the artifact `firmware-esp32s3` of the CI run, or make the report from your build.
