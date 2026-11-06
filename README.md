# mqtt-rs485-gateway

Gateway RS485 (Modbus RTU) → MQTT Sparkplug B 3.0 para la placa Waveshare **ESP32-S3-POE-ETH-8DI-8DO**, basado en **ESP-IDF**.

- `idf/`: firmware (ESP-IDF v6.1). Compilación, configuración, arquitectura y pruebas en `idf/README.md`.
- `docs/SPEC.md`: especificación de producto (completa y MVP).
- `docs/feasibility-idf.md`: viabilidad de cada requisito de la spec con ESP-IDF.
- `docs/review-idf.md`: review independiente del MVP, con hallazgos pendientes.
- `docs/RECOMMENDATION.md`: por qué se eligió ESP-IDF frente a Zephyr.

La implementación alternativa con Zephyr y la comparación completa se conservan en los branches `impl/zephyr` y `comparison/idf-vs-zephyr`.
