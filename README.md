# mqtt-rs485-gateway

This firmware is an RS485 (Modbus RTU) to MQTT Sparkplug B 3.0 gateway for the Waveshare **ESP32-S3-POE-ETH-8DI-8DO** board. It uses **ESP-IDF**.

- `idf/`: the firmware (ESP-IDF v6.1). `idf/README.md` gives the build, the configuration, the architecture and the tests.
- `docs/README.md`: the index of the documentation, with the main usage flows. Start here.
- `docs/tutorials/first-gateway-on-the-bench.md`: a tutorial, from a new board to data on the broker.
- `docs/SPEC.md`: the product specification (the full product and the MVP).
- `docs/feasibility-idf.md`: the feasibility of each requirement of the specification with ESP-IDF.
- `docs/review-idf.md`: an independent review of the MVP, with the open findings.
- `docs/RECOMMENDATION.md`: the reasons for ESP-IDF and not Zephyr.
- `docs/how-to/add-a-modbus-device.md`: how to add a Modbus device and its tags, with a test read first.
- `docs/how-to/change-the-configuration.md`: how to change the configuration, make a backup, and restore it.
- `docs/how-to/write-to-a-modbus-device.md`: how a Sparkplug host writes a value to a device or sets a digital output.
- `docs/how-to/update-the-firmware.md`: how to install a new image, examine the result, and go back to the earlier image.
- `docs/how-to/recover-the-gateway.md`: what to do in recovery mode, safe mode, with a lost web password, and how to do a factory reset.
- `docs/how-to/fit-the-rtc-battery.md`: how to install the RTC battery, which the gateway requires, and how to make sure that the RTC keeps the time.
- `docs/how-to/connect-the-rs485-bus.md`: how to connect the RS485 bus, and how to find the cause of a bus problem.
- `docs/how-to/connect-to-umh-core.md`: how to send the data of the gateway to UMH Core.
- `docs/how-to/decode-a-core-dump.md`: how to get the core dump of a gateway after a crash, and how to decode it.
- `docs/how-to/set-the-first-web-password.md`: how to set the first web password on site, without a computer at the panel.
- `docs/how-to/get-on-the-access-point.md`: how to open the web UI through the access point on a later visit.
- `docs/how-to/run-e2e-in-docker.md`: how to run the QEMU E2E tests in Docker, also on Windows.
- `docs/reference/e2e-durations.md`: the measured durations of the E2E tests on a Windows computer.
- `docs/reference/umh-names-and-topics.md`: the name limits of the gateway, and the UMH topic of each metric.
- `docs/reference/clock-and-rtc-battery.md`: the RTC battery, the sources of the clock, and the signals while the gateway waits for a valid clock.
- `docs/reference/status-signals.md`: the modes, the status LED, the buzzer and the BOOT button.
- `docs/reference/access-point.md`: the keys, the times, the signals, the REST API and the shell commands of the access point.
- `docs/reference/configuration.md`: all the keys of the configuration document, with their types, defaults and rules.
- `docs/reference/sparkplug-messages-and-metrics.md`: the Sparkplug topics, messages, metrics and commands of the gateway.
- `docs/reference/rest-api.md`: the routes of the REST API.
- `docs/reference/store-and-forward.md`: the RAM buffer, the microSD card, the retention and the duplicates.
- `docs/explanation/e2e-environment.md`: why the E2E tests on Windows run in Docker.
- `docs/explanation/access-point-security.md`: why the Wi-Fi password of the access point is the web password, and the residual risks.
- `docs/explanation/sparkplug-names-and-the-uns.md`: why the Sparkplug names control the place of the data in the UNS of UMH Core.

The branches `impl/zephyr` and `comparison/idf-vs-zephyr` keep the alternative Zephyr implementation and the full comparison.
