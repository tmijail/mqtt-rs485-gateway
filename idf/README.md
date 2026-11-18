# The firmware (ESP-IDF v6.1)

This folder has the firmware of the gateway, for the target `esp32s3`. The [project README](../README.md) tells what the gateway does and gives the status of the project.

> **Warning: The firmware has never run on the board (#1, #18).** It ran only in host tests and in QEMU.

| Folder | Contents |
|---|---|
| `main/` | The start of the firmware and its modes: normal, recovery and commissioning |
| `components/` | The components. `gw_core` has all the testable logic, in pure C without ESP-IDF. The other components are adapters to ESP-IDF |
| `test/host/` | The host tests (CMake, Unity, CTest) |
| `tools/e2e/` | The E2E harness: the real firmware in QEMU, against mosquitto and a pymodbus slave |
| `tools/` | The other tools: the screenshots of the docs, the Sparkplug decode check, the stack report and the linters |
| `cmake/` | The checks of the `sdkconfig` and the compiler options of the own components |
| `fs/` | The files of the LittleFS partition, with the example configuration |

Start with these documents:

- [Build the firmware](../docs/how-to/build-the-firmware.md) and [Build configuration](../docs/reference/build-configuration.md).
- [The architecture](../docs/reference/architecture.md) and [the reasons for it](../docs/explanation/architecture.md).
- [Run the tests](../docs/how-to/run-the-tests.md) and [Tests](../docs/reference/tests.md).
- [Known limitations](../docs/reference/known-limitations.md) and [Requirements status](../docs/reference/requirements-status.md).
- The [documentation index](../docs/README.md) for all the other documents.
