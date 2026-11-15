# Documentation

This folder has the documentation of the RS485 (Modbus RTU) to MQTT Sparkplug B gateway. The documents follow the Diátaxis structure: tutorials, how-to guides, reference and explanation.

> **Warning: The firmware has never run on the board (#1, #18).** The documents come from the firmware in host tests and in QEMU. Each document tells which parts are compile-only.

## The main flows

| I want to … | Document | Type |
|---|---|---|
| Learn the gateway on the bench, from a new board to data on the broker | [Your first gateway](tutorials/first-gateway-on-the-bench.md) | Tutorial |
| Connect the Modbus devices to the board | [Connect the RS485 bus](how-to/connect-the-rs485-bus.md) | How-to |
| Set the web password on site, without a computer at the panel | [Set the first web password on site](how-to/set-the-first-web-password.md) | How-to |
| Open the web UI through the Wi-Fi of the gateway on a later visit | [Get on the access point on a later visit](how-to/get-on-the-access-point.md) | How-to |
| Add a Modbus device and its tags | [Add a Modbus device and its tags](how-to/add-a-modbus-device.md) | How-to |
| Change the configuration, keep a copy, or load a copy | [Change, back up and restore the configuration](how-to/change-the-configuration.md) | How-to |
| Write a value to a device or set a digital output from a host | [Write a value to a Modbus device or a digital output](how-to/write-to-a-modbus-device.md) | How-to |
| Send the data to UMH Core | [Connect the gateway to UMH Core](how-to/connect-to-umh-core.md) | How-to |
| Install a new firmware image, or go back to the earlier one | [Update the firmware](how-to/update-the-firmware.md) | How-to |
| Repair a gateway that does not operate normally, or do a factory reset | [Recover a gateway](how-to/recover-the-gateway.md) | How-to |
| Install the RTC battery | [Fit the RTC battery](how-to/fit-the-rtc-battery.md) | How-to |
| Get the data of a crash | [Get and decode a core dump](how-to/decode-a-core-dump.md) | How-to |

The usual sequence for a new gateway is:

1. On the bench: flash the firmware and load the configuration of the site (the tutorial, then [Change, back up and restore the configuration](how-to/change-the-configuration.md)).
2. On site: connect the bus and the network, and set the web password ([Connect the RS485 bus](how-to/connect-the-rs485-bus.md), [Set the first web password on site](how-to/set-the-first-web-password.md)).
3. Add or correct the devices ([Add a Modbus device and its tags](how-to/add-a-modbus-device.md)).
4. Make a backup of the configuration.
5. Later: update the firmware, and recover the gateway if necessary.

## Reference

- [The status signals](reference/status-signals.md): the modes, the LED, the buzzer and the BOOT button.
- [The access point](reference/access-point.md): the keys, the times, the signals, the REST API and the shell commands of the access point.
- [The clock and the RTC battery](reference/clock-and-rtc-battery.md): the sources of the clock, and the signals while the gateway waits for a valid clock.
- [UMH names and topics](reference/umh-names-and-topics.md): the name limits of the gateway, and the UMH topic of each metric.
- [Sparkplug TCK results](reference/sparkplug-tck-results.md): the results of the Eclipse Sparkplug TCK, Edge Node profile.
- `SPEC.md`: the product specification, with the configuration schema (§6) and the REST API (§6.1).
- [`idf/README.md`](../idf/README.md): the architecture, the build, the configuration, the tests and the known limits.

## Explanation

- [The security of the access point](explanation/access-point-security.md): why the Wi-Fi password of the access point is the web password, and the residual risks.
- [Sparkplug names and the UNS](explanation/sparkplug-names-and-the-uns.md): why the Sparkplug names control the place of the data in the UNS of UMH Core.

## For developers

- [Run the E2E tests in Docker](how-to/run-e2e-in-docker.md), [The E2E environment on a Windows computer](explanation/e2e-environment.md) and [E2E durations](reference/e2e-durations.md).
- [Run the Sparkplug TCK against the gateway](how-to/run-the-sparkplug-tck.md).
- [Make the screenshots of the web UI for the docs](how-to/make-the-docs-screenshots.md).
- `feasibility-idf.md`, `review-idf.md`, `RECOMMENDATION.md`, `review-feature-zephyr-ideas.md` and `upstream-reports.md`: the studies and the reports of the project.
