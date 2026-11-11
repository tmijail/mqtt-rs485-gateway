# mqtt-rs485-gateway

This firmware is an RS485 (Modbus RTU) to MQTT Sparkplug B 3.0 gateway for the Waveshare **ESP32-S3-POE-ETH-8DI-8DO** board. It uses **ESP-IDF**.

- `idf/`: the firmware (ESP-IDF v6.1). `idf/README.md` gives the build, the configuration, the architecture and the tests.
- `docs/SPEC.md`: the product specification (the full product and the MVP).
- `docs/feasibility-idf.md`: the feasibility of each requirement of the specification with ESP-IDF.
- `docs/review-idf.md`: an independent review of the MVP, with the open findings.
- `docs/RECOMMENDATION.md`: the reasons for ESP-IDF and not Zephyr.
- `docs/how-to/connect-the-rs485-bus.md`: how to connect the RS485 bus, and how to find the cause of a bus problem.
- `docs/how-to/connect-to-umh-core.md`: how to send the data of the gateway to UMH Core.
- `docs/how-to/decode-a-core-dump.md`: how to get the core dump of a gateway after a crash, and how to decode it.
- `docs/how-to/run-e2e-in-docker.md`: how to run the QEMU E2E tests in Docker, also on Windows.
- `docs/reference/e2e-durations.md`: the measured durations of the E2E tests on a Windows computer.
- `docs/reference/umh-names-and-topics.md`: the name limits of the gateway, and the UMH topic of each metric.
- `docs/explanation/e2e-environment.md`: why the E2E tests on Windows run in Docker.
- `docs/explanation/sparkplug-names-and-the-uns.md`: why the Sparkplug names control the place of the data in the UNS of UMH Core.

The branches `impl/zephyr` and `comparison/idf-vs-zephyr` keep the alternative Zephyr implementation and the full comparison.
