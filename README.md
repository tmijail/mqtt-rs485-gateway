# mqtt-rs485-gateway

This firmware is an RS485 (Modbus RTU) to MQTT Sparkplug B 3.0 gateway for the Waveshare **ESP32-S3-POE-ETH-8DI-8DO** board. It uses **ESP-IDF**.

- `idf/`: the firmware (ESP-IDF v6.1). `idf/README.md` gives the build, the configuration, the architecture and the tests.
- `docs/SPEC.md`: the product specification (the full product and the MVP).
- `docs/feasibility-idf.md`: the feasibility of each requirement of the specification with ESP-IDF.
- `docs/review-idf.md`: an independent review of the MVP, with the open findings.
- `docs/RECOMMENDATION.md`: the reasons for ESP-IDF and not Zephyr.

The branches `impl/zephyr` and `comparison/idf-vs-zephyr` keep the alternative Zephyr implementation and the full comparison.
