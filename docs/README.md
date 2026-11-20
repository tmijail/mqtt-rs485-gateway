# Documentation

This is the documentation of the RS485 (Modbus RTU) to MQTT Sparkplug B gateway. The documents follow the Diátaxis structure: tutorials, how-to guides, reference and explanation. You can read them on the [documentation site](https://mijail.ar/mqtt-rs485-gateway/) or in the folder `docs/` of the repository.

> **Warning: The firmware has never run on the board (#1, #18).** The documents come from the firmware in host tests and in QEMU. Each document tells which parts are compile-only.

## The main flows

| I want to … | Document | Type |
|---|---|---|
| Learn the gateway on the bench, from a new board to data on the broker | [Your first gateway](tutorials/first-gateway-on-the-bench.md) | Tutorial |
| Connect the Modbus devices to the board | [Connect the RS485 bus](how-to/connect-the-rs485-bus.md) | How-to |
| Set the web password on site, without a computer at the panel | [Set the first web password on site](how-to/set-the-first-web-password.md) | How-to |
| Change the web password (also the Wi-Fi password of the access point) | [Change the web password](how-to/change-the-web-password.md) | How-to |
| Open the web UI through the Wi-Fi of the gateway on a later visit | [Get on the access point on a later visit](how-to/get-on-the-access-point.md) | How-to |
| Add a Modbus device and its tags | [Add a Modbus device and its tags](how-to/add-a-modbus-device.md) | How-to |
| Keep the tag list of a device model one time, for two or more devices | [Add devices of one type](how-to/add-devices-of-one-type.md) | How-to |
| Connect the gateway to a Wi-Fi network, as a backup of Ethernet or as the primary interface | [Connect the gateway to a Wi-Fi network](how-to/connect-the-gateway-to-wi-fi.md) | How-to |
| Use the NTP server of the site, and make sure that the clock is valid | [Set the time servers](how-to/set-the-time-servers.md) | How-to |
| Encrypt the MQTT connection, with the CA of the broker and a client certificate | [Connect to a broker with TLS](how-to/connect-to-a-broker-with-tls.md) | How-to |
| Load the HTTPS certificate of the plant, and load, replace or remove the PEM files | [Manage the certificates](how-to/manage-the-certificates.md) | How-to |
| Change the configuration, keep a copy, or load a copy | [Change, back up and restore the configuration](how-to/change-the-configuration.md) | How-to |
| Write a value to a device or set a digital output from a host | [Write a value to a Modbus device or a digital output](how-to/write-to-a-modbus-device.md) | How-to |
| Set the DIs and DOs of the board (inversion, debounce, fast counters, pulse DOs, run hours, local rules), or set a DO from the web UI | [Set up the local inputs and outputs](how-to/set-up-the-local-io.md) | How-to |
| Give a PLC or a SCADA access to the RS485 devices and to the I/O of the gateway with Modbus TCP | [Connect a Modbus TCP client](how-to/connect-a-modbus-tcp-client.md) | How-to |
| Send the data to UMH Core | [Connect the gateway to UMH Core](how-to/connect-to-umh-core.md) | How-to |
| Install a new firmware image, or go back to the earlier one | [Update the firmware](how-to/update-the-firmware.md) | How-to |
| Repair a gateway that does not operate normally, or do a factory reset | [Recover a gateway](how-to/recover-the-gateway.md) | How-to |
| Stop the polling and MQTT for a time, or erase the gateway from a distance | [Restart into safe mode or do a factory reset](how-to/restart-into-safe-mode-or-reset.md) | How-to |
| Read the log in the browser, or get more log lines from one part of the firmware | [Read the live log](how-to/read-the-live-log.md) | How-to |
| Find one gateway in a panel with a number of gateways | [Find the gateway in a panel](how-to/find-the-gateway-in-a-panel.md) | How-to |
| Install the RTC battery | [Fit the RTC battery](how-to/fit-the-rtc-battery.md) | How-to |
| Get the data of a crash | [Get and decode a core dump](how-to/decode-a-core-dump.md) | How-to |

The usual sequence for a new gateway is:

1. On the bench: flash the firmware and load the configuration of the site (the tutorial, then "Change, back up and restore the configuration").
2. On site: connect the bus and the network, and set the web password ("Connect the RS485 bus", "Set the first web password on site").
3. Add or correct the devices ("Add a Modbus device and its tags").
4. Make a backup of the configuration.
5. Later: update the firmware, and recover the gateway if necessary.

## Reference

- [The configuration document](reference/configuration.md): all the keys of the configuration, with their types, defaults and rules, and the datatypes of the tags in Sparkplug.
- [Sparkplug messages and metrics](reference/sparkplug-messages-and-metrics.md): the topics, the messages, the node metrics and their aliases, the device metrics and the commands.
- [The REST API](reference/rest-api.md): the authentication, the routes and the responses of the REST API.
- [Store & forward](reference/store-and-forward.md): the RAM buffer, the microSD card, the retention and the duplicates after a restart.
- [The status signals](reference/status-signals.md): the modes, the LED, the buzzer and the BOOT button.
- [The access point](reference/access-point.md): the keys, the times, the signals, the REST API and the shell commands of the access point.
- [The clock and the RTC battery](reference/clock-and-rtc-battery.md): the sources of the clock, and the signals while the gateway waits for a valid clock.
- [UMH names and topics](reference/umh-names-and-topics.md): the name limits of the gateway, and the UMH topic of each metric.
- [Sparkplug behavior](reference/sparkplug-behavior.md): the choices of the gateway where the Sparkplug specification permits a choice, with the reasons.
- [Commissioning and secrets](reference/commissioning-and-secrets.md): the modes, the HTTPS certificate, the claim window, the secrets in NVS, the factory reset and the limits of the web server.
- [Firmware updates](reference/firmware-updates.md): the download, the pre-flight check, the trial, the rollback and the limits of the updates.
- [Requirements status](reference/requirements-status.md): the status of each requirement of the specification, with the evidence.
- [Known limitations](reference/known-limitations.md): the limits of the gateway, grouped by subsystem.
- [Board data to verify](reference/board-data-to-verify.md): the data of the board that nobody verified on a board.

## Explanation

- [The security of the access point](explanation/access-point-security.md): why the Wi-Fi password of the access point is the web password, and the residual risks.
- [Sparkplug names and the UNS](explanation/sparkplug-names-and-the-uns.md): why the Sparkplug names control the place of the data in the UNS of UMH Core.
- [The security of the web UI and the claim](explanation/web-and-claim-security.md): why the web server has its limits, and why the claim window is safe.
- [Features that burn eFuses](explanation/no-efuse-features.md): why Secure Boot, flash encryption and the other eFuse features are not in `main`.

## For developers

- [Build the firmware](how-to/build-the-firmware.md) and [Build configuration](reference/build-configuration.md): ESP-IDF, the build, the flash, the `sdkconfig` overlays and the partition table.
- [The product specification](SPEC.md): the requirements (for example MB-6, SPB-14 and FW-1) that the code, the tests and the commits refer to.
- [The architecture](reference/architecture.md) and [the reasons for it](explanation/architecture.md): the components, the tasks, the concurrency rules and the memory budget.
- [Run the tests](how-to/run-the-tests.md) and [Tests](reference/tests.md): the host tests, the linters, the E2E tests, the CI jobs and where the results of a run are.
- [The E2E harness](reference/e2e-harness.md) and [The E2E scenarios](reference/e2e-scenarios.md): the test bench in QEMU, the retry rules, and what each scenario examines.
- [Run the E2E tests in Docker](how-to/run-e2e-in-docker.md) and [the reasons for it](explanation/e2e-environment.md): the E2E tests on a Windows computer, in the ESP-IDF image of the CI.
- [Compiler warnings and stack data](reference/compiler-warnings-and-stack.md): the warning options, the stack report and the limit of a stack frame.
- [The core dump](explanation/core-dump.md): why the gateway keeps the core dump as it does.
- [Run the Sparkplug TCK against the gateway](how-to/run-the-sparkplug-tck.md).
- [Make the screenshots of the web UI for the docs](how-to/make-the-docs-screenshots.md).
- [Build the documentation site](how-to/build-the-documentation-site.md): Zensical, the local server, the strict build and the rules for links.
- The [issues with the label `upstream`](https://github.com/tmijail/mqtt-rs485-gateway/issues?q=label%3Aupstream): the defects of other projects (esp-mqtt, esp-tls, the QEMU of Espressif), with the text of each report.
- The tag `archive/studies` ([commit d645ec6](https://github.com/tmijail/mqtt-rs485-gateway/tree/d645ec6df5c11da5d2e22fdeb5151bb00bfdd88d/docs)): the studies of the project before the implementation (feasibility, reviews and the selection of ESP-IDF), in Spanish.
- The branches `impl/zephyr` and `comparison/idf-vs-zephyr` keep the alternative Zephyr implementation and the full comparison.
