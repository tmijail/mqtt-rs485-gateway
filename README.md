# mqtt-rs485-gateway — implementación ESP-IDF (snapshot)

Gateway RS485 (Modbus RTU) → MQTT Sparkplug B para la placa Waveshare **ESP32-S3-POE-ETH-8DI-8DO**.

Este branch guarda **la implementación del MVP con ESP-IDF tal como se entregó**, sin cambios posteriores:

- `docs/SPEC.md`: especificación de producto (completa y MVP)
- `idf/`: implementación del MVP con ESP-IDF v6.1 (ver `idf/README.md`)
- `docs/feasibility-idf.md`: viabilidad de cada requisito con ESP-IDF
- `docs/review-idf.md`: review independiente

La comparación con Zephyr está en el branch `comparison/idf-vs-zephyr`; la línea de desarrollo activa, en `main`.
