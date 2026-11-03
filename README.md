# mqtt-rs485-gateway — implementación Zephyr (snapshot)

Gateway RS485 (Modbus RTU) → MQTT Sparkplug B para la placa Waveshare **ESP32-S3-POE-ETH-8DI-8DO**.

Este branch guarda **la implementación del MVP con Zephyr RTOS tal como se entregó**:

- `docs/SPEC.md`: especificación de producto (completa y MVP)
- `zephyr/`: implementación del MVP con Zephyr v4.5.0 (ver `zephyr/README.md`)
- `docs/feasibility-zephyr.md`: viabilidad de cada requisito con Zephyr
- `docs/review-zephyr.md`: review independiente

La comparación con ESP-IDF está en el branch `comparison/idf-vs-zephyr`. Se eligió ESP-IDF como base del producto (branch `main`).
