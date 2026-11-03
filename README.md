# mqtt-rs485-gateway — comparación ESP-IDF vs Zephyr

Gateway RS485 (Modbus RTU) → MQTT Sparkplug B para la placa Waveshare **ESP32-S3-POE-ETH-8DI-8DO**.

Este branch reúne las dos implementaciones del MVP y su evaluación:

- `docs/SPEC.md`: especificación de producto (completa y MVP)
- `idf/` y `zephyr/`: implementaciones del MVP, desarrolladas por separado
- `docs/feasibility-{idf,zephyr}.md`: viabilidad de cada requisito por stack
- `docs/review-{idf,zephyr}.md`: reviews independientes
- `docs/RECOMMENDATION.md`: comparación y recomendación de stack (ESP-IDF)

Cada implementación también está sola en `impl/idf` e `impl/zephyr`. El desarrollo sigue en `main`.
