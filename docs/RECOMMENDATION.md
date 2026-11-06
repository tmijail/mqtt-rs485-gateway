# Comparación de implementaciones y recomendación de stack

Fecha: 2026-11-03

> En `main` solo están la implementación IDF y sus documentos. `zephyr/`, `docs/feasibility-zephyr.md` y `docs/review-zephyr.md`, que se citan abajo, están en el branch `comparison/idf-vs-zephyr`.

**Insumos:**
- `docs/SPEC.md`;
- los estudios de viabilidad `docs/feasibility-{idf,zephyr}.md`;
- las reviews independientes `docs/review-{idf,zephyr}.md`;
- la revisión del código de `idf/` y `zephyr/` que hizo el coordinador.

## 1. Veredicto

**Recomiendo ESP-IDF (v6.1) para este producto y esta placa.**

Las dos implementaciones son de buen nivel y casi empatan en el MVP. La implementación Zephyr tiene la arquitectura más declarativa y la mejor base de pruebas. Lo que inclina la balanza es lo que viene después del MVP: lo que la spec llama "de buen nivel" en un producto industrial.

- **Seguridad de plataforma.** Secure boot (FW-2) y cifrado de flash (FW-3) son nativos e integrados en IDF. En Zephyr, el propio estudio de viabilidad los clasifica como *"No viable / muy riesgoso (vía Zephyr)"*: hay que salir del flujo normal y pasar por el port Espressif de MCUboot.
- **El hardware del ESP32-S3.**
  - **RS485:** IDF usa el modo half-duplex por hardware de la UART. Zephyr no puede, porque su subsistema Modbus pisa el control de flujo.
  - **Segundo núcleo:** IDF fija las tareas a núcleos. Zephyr corre solo en el PRO CPU.
  - **Wi-Fi:** en Zephyr depende de blobs que en este entorno ni siquiera se pudieron descargar, así que no se compiló nada inalámbrico.
- **Funciones P1 que en IDF ya existen:** OTA con *rollback* (`esp_https_ota`), cifrado de NVS con clave en eFuse por HMAC (CFG-7), aprovisionamiento por SoftAP y BLE, y un cliente MQTT que gestiona keepalive y TLS. En Zephyr todo esto es "Nativo + trabajo propio".
- **Defectos.** Los de la implementación IDF son de lógica de aplicación y se corrigen en pocas líneas (§4). El defecto Medium más profundo de Zephyr, no detectar sesiones TCP medio abiertas, viene de que su cliente MQTT es de bajo nivel. Ese costo se repite en cada función de red que se agregue.

Zephyr sería la opción correcta si el producto tuviera que **portarse a otros MCU**, por ejemplo una futura versión sobre STM32 o nRF con el mismo firmware. También si el equipo ya domina Zephyr y está dispuesto a mantener por su cuenta la integración de seguridad de Espressif.

## 2. Evidencia reproducida

Los reviewers reprodujeron todo en directorios propios.

| | ESP-IDF v6.1 | Zephyr v4.5.0 |
|---|---|---|
| Build para ESP32-S3 | 0 warnings (`-Wall -Wextra -Werror`) | 0 warnings |
| Imagen / RAM | 714 KB de imagen, DIRAM 44 % | 422 KB de flash, DRAM 54,5 % |
| Tests unitarios en el host | 54/54 (9 suites, CTest + Unity) | 32/32 (ztest en `native_sim`) |
| Interoperabilidad Sparkplug (`protobuf` de Python con el `.proto` oficial de Tahu) | 5 payloads, 0 errores | 19 + 6 payloads capturados en vivo, 0 errores |
| Punta a punta contra mosquitto y pymodbus | 22/22 en **QEMU** con el firmware real del S3 (NIC openeth) | 30/30 en **native_sim** (sockets del host, UART por pty) |
| Criterios de aceptación §7 | los 6 | los 6 |
| Probado en placa | No | No |

Las dos ejecutan el firmware real en emulación. El E2E de IDF corre el binario Xtensa en QEMU; el de Zephyr corre el mismo código compilado para el host. Ninguna de las dos ejercita el W5500, la UART real, el TCA9554 ni el LED.

## 3. Comparación por eje

Puntajes de 1 a 10. Entre paréntesis va el puntaje del reviewer, cuando mi evaluación difiere.

| Eje | IDF | Zephyr | Comentario |
|---|---|---|---|
| Completitud MVP | 9 | 8 | Zephyr nunca emite FC15 (MB-4 parcial) y no tiene tests de CRC ni de tramas, porque delega en el subsistema |
| Corrección Sparkplug | 9 | 9 | Las dos son correctas en bdSeq, seq, aliases, BIRTH/DEATH, rebirth y DCMD. IDF además reemite los BIRTH cuando SNTP sincroniza tarde |
| Corrección Modbus | 7 | 7 | IDF tiene trama propia con lectura en bloques y modo RS485 por hardware, pero deja valores congelados marcados como buenos y falla con FC16 a 1200 baud. Zephyr hace una transacción por tag (ciclos lentos con muchos tags) |
| Robustez / potencial de bugs | **6** (7) | 6 | IDF: polaridad de las DO invertida (§4.1) y caída silenciosa a la configuración de ejemplo. Zephyr: no detecta TCP medio abierto y acepta claves JSON duplicadas |
| Aprovechamiento del hardware | 7 | 6 | IDF: UART en modo RS485, PSRAM, afinidad de núcleos y lecturas en bloque. Zephyr: DE por GPIO, un solo núcleo y un tag por transacción |
| Idiomaticidad del stack | 8 | 9 | Zephyr tiene una definición de placa propia, con pines y polaridades en el devicetree, settings sobre ZMS y task_wdt respaldado por el watchdog de hardware |
| Legibilidad / elegancia | 9 | 8 | IDF separa un núcleo en C puro (`gw_core`) con *ports* inyectados. Zephyr es muy legible, pero tiene un parser JSON propio de 425 LOC |
| Testabilidad / evidencia | 8 | 8 | IDF cubre trama y CRC; Zephyr tiene el E2E más completo (30 checks, reinicio del broker) |
| **Promedio** | **7,9** | **7,6** | |

**Tamaño del código** (C y headers, sin vendorizado ni generado): son comparables. IDF tiene unas 4,6 k líneas de aplicación y 2,0 k de tests; Zephyr, unas 5,0 k de aplicación, 1,8 k de tests y 0,5 k de definición de placa. Zephyr escribe menos código de Modbus porque usa el subsistema, pero más de MQTT (keepalive, poll y reconexión a mano) y de JSON.

### Expresividad y elegancia

- **Zephyr.**
  - La placa queda descrita **declarativamente**: `di-gpios` y `do-gpios` con `GPIO_ACTIVE_LOW`, el W5500, el `pca95xx`, el WS2812 por RMT y `de-gpios`. El código de E/S no sabe nada de polaridades ni de pines. Es lo más elegante de las dos implementaciones, y es justamente lo que evitó el bug de las DO.
  - El modelo de "estado coalescido + eventfd" es simple y acotado por construcción.
- **IDF.**
  - El mejor diseño es la separación **lógica pura / HAL**. `gw_core` (configuración, trama Modbus, conversión, plan de bloques, encuesta, motor Sparkplug) compila y se prueba en el host sin IDF.
  - La tarea `sparkplug` es un *reconciler*: lleva el estado al que indican los flags atómicos. Es una buena forma de evitar carreras entre el evento MQTT y la lógica.
  - El costo es más configuración en Kconfig que una descripción del hardware.

### Potencial de bugs

- **Comunes a las dos.** Los dos reviewers los marcaron solo en una, pero los verifiqué en la otra:
  - **Dispositivos fuera de línea saturan el bus.** Se sondean a la tasa normal con timeouts y reintentos completos, unos 0,6 s por sondeo con los valores por defecto (`gw_poll.c`, `mb_poller.c`). Ninguna implementa un *backoff* de sondeo.
  - **Tormenta de reconexión.** El contador de backoff vuelve a cero con el CONNACK. Un broker que acepta y corta provoca un bucle de unos 1 s, con una escritura de bdSeq en flash por intento.
  - **Watchdog durante un rebirth.** NBIRTH más 16 DBIRTH sin alimentar el watchdog, con envíos bloqueantes de 5 a 10 s, pueden exceder el timeout: 15 s en IDF y 30 s en Zephyr.
- **Propios de Zephyr.** Vienen de usar primitivas de bajo nivel: el keepalive del cliente MQTT sin verificar `unacked_ping` y el parser JSON propio. Aparecen cada vez que hay que reconstruir algo que IDF trae resuelto.
- **Propios de IDF.** Son decisiones de lógica en código propio: el *fallback* de la configuración, la validez por bloque y el timeout fijo de TX. No vienen del stack.

## 4. Hallazgos a corregir antes de cualquier prueba en placa

### 4.1 [Alta, lo encontró el coordinador] IDF: polaridad de las DO invertida, que encendería las 8 salidas al arrancar

**Dónde:** `idf/components/gw_board/gw_io.c`. El arranque escribe `OUTPUT = 0x00` y después `CONFIG = 0x00`, y `gw_io_set_do(n, true)` pone el bit en 1. Es decir, el código asume nivel alto = salida activa.

**Evidencia en contra**, revisada en el demo oficial (`WS_Dout.cpp`, `WS_DIN.cpp`) y en ESPHome:

1. `Dout_Init()` llama a `TCA9554PWR_Init(0x00, 0xFF)`: el demo arranca con **todas las salidas en nivel alto**. Un demo no energiza 8 cargas al encender.
2. En el modo por defecto (`DIN_Inverse_Enable = 0`), el enlace DI→DO copia el nivel crudo. La DI en reposo está en 1 por el pull-up y la DO queda en 1; con la DI energizada ambas pasan a 0. Eso solo es coherente si **nivel bajo = DO activa**.
3. ESPHome declara las 8 DO de esta placa con `inverted: true`.

La implementación Zephyr llegó a la misma conclusión (`GPIO_ACTIVE_LOW` en el DTS). El reviewer de IDF no lo detectó.

**Efecto, si se confirma:** cada arranque, incluido el *watchdog reset*, **activa las 8 salidas** hasta que llega un NCMD, y `DO/n=true` las apaga. En una placa que maneja cargas de 500 mA es un riesgo de seguridad.

**Corrección:** agregar un Kconfig `GW_DO_ACTIVE_LOW` (por defecto `y`), cargar el latch con `0xFF` antes de configurar los pines como salida e invertir el bit en `gw_io_set_do` y en la relectura. Lo primero de la puesta en marcha debe ser medir esto con un multímetro.

### 4.2 Resto de los hallazgos

| Sev. | Stack | Hallazgo | Fuente |
|---|---|---|---|
| Alta | IDF | Con la configuración ilegible, de más de 32 KB, o con LittleFS sin montar, arranca con el JSON de ejemplo (Plant1/GW01). Viola CFG-1 y puede chocar `client_id` en el broker | review-idf H1 |
| Media | IDF | Si un bloque falla y otro responde, los tags del bloque caído siguen marcados como válidos: SCADA ve un valor congelado como GOOD | review-idf |
| Media | IDF | `uart_wait_tx_done` fijo en 100 ms: FC16 siempre falla a 1200 baud, pero el esclavo ejecuta la escritura (hasta 3 veces) | review-idf |
| Media | Zephyr | Una sesión MQTT medio abierta nunca se detecta (no se verifica `unacked_ping`). Con el broker congelado 60 s sigue figurando "connected" | review-zephyr |
| Media | Zephyr | Claves JSON duplicadas aceptadas: se solapan rangos de tags y el mismo alias aparece en dos DBIRTH | review-zephyr |
| Media | Ambos | Los dispositivos fuera de línea saturan el bus (§3) | reviews y coordinador |
| Media | Ambos | El watchdog puede vencer durante un rebirth sobre un enlace lento | reviews |
| Baja | Ambos | Tormenta de reconexión con escritura de bdSeq en flash por intento | review-zephyr y coordinador |
| Baja | Ambos | Límite de 32 KB para el JSON, cuando 256 tags (legal según §6) ocupan unos 39 KB | review-idf y coordinador |

## 5. Próximos pasos sugeridos

1. **Adoptar `idf/` como base** y corregir los hallazgos de §4. Ninguno pasa de unas decenas de líneas.
2. **Traer de Zephyr las buenas ideas.**
   - **Descripción de la placa:** concentrar pines y polaridades en un único header o Kconfig de placa, para evitar otro error como el de las DO.
   - **Pruebas de punta a punta:** sumar al E2E de QEMU los escenarios de reinicio del broker y de broker congelado.
3. **Campaña en placa**, en este orden:
   - polaridad de las DO y las DI;
   - consola (USB nativo o CH34x);
   - W5500 (enlace y reconexión);
   - tiempos de DE/RTS a 115200 baud con osciloscopio;
   - orden de color del LED;
   - PSRAM octal.
4. **Roadmap P1 en IDF:**
   - TLS y mTLS (MQTT-4/5);
   - OTA con rollback confirmado tras el NBIRTH (FW-1);
   - NVS cifrado para los secretos (CFG-7);
   - host primario / STATE (SPB-13);
   - store & forward en PSRAM (SPB-14);
   - failsafe de las DO (IO-3);
   - interfaz web (CFG-3).
   
   Al final, secure boot v2 y cifrado de flash, con una política de claves definida, porque son irreversibles.
