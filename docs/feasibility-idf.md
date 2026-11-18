# Viabilidad con ESP-IDF — Gateway RS485 (Modbus RTU) → MQTT Sparkplug B

Versión: 1.0 · Fecha: 2026-11-03 · Alcance: todos los requisitos de `docs/SPEC.md` §4.

## 0. Base verificada

Todo lo que sigue se comprobó contra el código instalado, no de memoria.

- **ESP-IDF v6.1** (última estable a la fecha; tags `v6.0.x` y `v6.1`), clonado con `--depth 1 --shallow-submodules` en `/home/user/sdk/idf/esp-idf`. Toolchain `xtensa-esp-elf 15.2.0_20251204`.
- **Registro de componentes**: consultado en `components.espressif.com` el 2026-11-03.
- **Cambios de IDF 6.0 que afectan a este producto** (ver `docs/en/migration-guides/release-6.x/6.0/*.rst`):
  - **esp-mqtt salió del núcleo**: ahora es `espressif/mqtt` (1.1.0).
  - **El componente `json` desapareció**: ahora es `espressif/cjson` (1.7.19~2).
  - **Los drivers Ethernet SPI (W5500, KSZ8851SNL, DM9051) salieron de `esp_eth`**: ahora están en `esp-eth-drivers`, con `espressif/w5500` 2.0.1 (depende de `espressif/wiznet_common`). En `components/esp_eth/src/mac` solo quedan la EMAC interna y `openeth`.
  - **`wifi_provisioning` se renombró**: ahora es `espressif/network_provisioning` (1.4.0).

Leyenda de **Viabilidad**:

- **Nativo**: lo provee ESP-IDF.
- **Componente oficial**: registro de Espressif.
- **Trabajo propio**: hay que escribirlo.
- **Terceros**: componente o librería de otro autor.
- **No viable / muy riesgoso**.

**Esfuerzo**: S ≤ 2 días · M ≤ 1–2 semanas · L > 2 semanas.

Columna **MVP**: ✅ = implementado en `idf/` (ver [`docs/reference/requirements-status.md`](reference/requirements-status.md)).

## 1. Red (NET)

| ID | Viabilidad | Mecanismo concreto (IDF 6.1) | Esf. | Riesgos / notas |
|---|---|---|---|---|
| NET-1 ✅ | Componente oficial + Nativo | `espressif/w5500` 2.0.1: `esp_eth_mac_new_w5500()`/`esp_eth_phy_new_w5500()` sobre `spi_master` (SPI2, MOSI 13, MISO 14, SCLK 15, CS 16, INT 12, RST 39). `esp_eth_driver_install`, `esp_eth_new_netif_glue`, `esp_netif`; eventos `ETHERNET_EVENT_CONNECTED/DISCONNECTED`, `IP_EVENT_ETH_GOT_IP/LOST_IP`. La reconexión del enlace la hace el driver (sondeo del PHY). | S | El W5500 no tiene MAC propia: se usa `esp_read_mac(ESP_MAC_ETH)` + `ETH_CMD_S_MAC_ADDR`. Requiere `gpio_install_isr_service()` antes de instalar el driver (la ISR la registra `wiznet_common`). Desde 6.0 es un componente aparte: hay que fijar la versión. |
| NET-2 ✅ | Nativo | DHCP por defecto en `ESP_NETIF_DEFAULT_ETH()`. Estática: `esp_netif_dhcpc_stop()` antes de arrancar, `esp_netif_set_ip_info()`, `esp_netif_set_dns_info(ESP_NETIF_DNS_MAIN)`. | S | `dhcpc_stop` funciona en estado INIT (verificado en `esp_netif_lwip.c`). |
| NET-3 ✅ | Nativo | lwIP DNS. esp-mqtt resuelve con `getaddrinfo` (esp-tls/tcp_transport) en cada conexión. | S | Con DHCP se usa el DNS que entrega el servidor. |
| NET-4 | Nativo | `esp_wifi` STA y un segundo `esp_netif` (`ESP_NETIF_DEFAULT_WIFI_STA`). La prioridad se fija con `esp_netif_inherent_config_t.route_prio` o con `esp_netif_set_default_netif()`. | M | Por defecto STA tiene `route_prio` 100 y ETH 50, así que **Wi-Fi ganaría la ruta por defecto**: hay que invertirlo. La sesión MQTT no migra sola entre interfaces. Hay que cortar y reconectar, con un bdSeq nuevo y otro NBIRTH. Wi-Fi + PSRAM + TLS presionan la RAM interna. |
| NET-5 | Componente oficial / Nativo | SoftAP con `esp_wifi` + `esp_http_server` (portal propio), o `espressif/network_provisioning` 1.4.0 (protocomm por SoftAP/BLE). BOOT = GPIO0. | M | GPIO0 es pin de *strapping*: leerlo solo después del arranque. `network_provisioning` está pensado para la app móvil de Espressif. Para un instalador industrial conviene un portal web propio. |
| NET-6 | Componente oficial | `espressif/mdns` 1.14.1 (`mdns_init`, `mdns_hostname_set`, `mdns_service_add("_http","_tcp")`), `esp_netif_set_hostname()`. | S | — |
| NET-7 | Nativo | lwIP `CONFIG_LWIP_IPV6` (activo por defecto), SLAAC `CONFIG_LWIP_IPV6_AUTOCONFIG`, `esp_netif_create_ip6_linklocal()`, evento `IP_EVENT_GOT_IP6`. | S–M | El esquema §6 no tiene campos IPv6: habría que extenderlo. W5500 en modo MACRAW soporta IPv6 (el stack es lwIP). |

## 2. Tiempo (TIME)

| ID | Viabilidad | Mecanismo concreto | Esf. | Riesgos / notas |
|---|---|---|---|---|
| TIME-1 ✅ | Nativo | `esp_netif_sntp_init(ESP_NETIF_SNTP_DEFAULT_CONFIG(server))`, `esp_netif_sntp_start()` al obtener IP, `sync_cb`. Timestamps con `gettimeofday()` en ms UTC. | S | **Encontrado en la prueba**: `CONFIG_LWIP_SNTP_STARTUP_DELAY` (activo por defecto, hasta 5 s aleatorios, RFC 4330) retrasa la primera sincronización más allá del NBIRTH. Se desactiva en `sdkconfig.defaults` y, si aun así se publica antes de sincronizar, se hace un *rebirth* al sincronizar. |
| TIME-2 | Trabajo propio / Terceros | PCF85063 (dirección 0x51 según el demo de Waveshare) sobre `esp_driver_i2c` (`i2c_master_transmit_receive`), en el mismo bus que el TCA9554 (SDA 42, SCL 41). No hay componente de Espressif. En el registro existen `espp/pcf85063` (C++) y `waveshare/pcf85063a`. Al arrancar, `settimeofday()` desde el RTC; tras cada SNTP, escribir el RTC. | S | El bus I²C se comparte con las DO: hay que serializarlo (mutex o un único dueño). Hay que comprobar el bit OS (*oscillator stop*) para no confiar en una hora inválida. |
| TIME-3 | Trabajo propio | `sntp_get_sync_status()` / flag propio → métrica Boolean en NBIRTH/NDATA. | S | — |

## 3. MQTT

| ID | Viabilidad | Mecanismo concreto | Esf. | Riesgos / notas |
|---|---|---|---|---|
| MQTT-1 ✅ | Componente oficial | `espressif/mqtt` 1.1.0: `esp_mqtt_client_config_t` (`broker.address.hostname/port`, `credentials.client_id/username/authentication.password`, `session.keepalive`, `session.protocol_ver = MQTT_PROTOCOL_V_3_1_1`). | S | Desde IDF 6.0 ya no es parte del núcleo: hay que fijar la versión en `idf_component.yml`. |
| MQTT-2 ✅ | Componente oficial + Trabajo propio | esp-mqtt solo tiene un `reconnect_timeout_ms` fijo. Se usa `network.disable_auto_reconnect = true` y la tarea propia llama a `esp_mqtt_client_reconnect()` (que solo actúa en `MQTT_STATE_WAIT_RECONNECT`, verificado en `mqtt_client.c`) tras un backoff exponencial con jitter. | S | Verificado en QEMU: retardos 1000 → 1664 → 3308 → 7581 ms. |
| MQTT-3 ✅ | Componente oficial | `session.last_will` (topic, msg, `msg_len`, qos 1, retain 0). Para cambiar el will antes de cada intento: `esp_mqtt_set_config()`, que copia el mensaje con `malloc` y toma `MQTT_API_LOCK`. | S | El will debe cambiar con cada bdSeq. Hacerlo con auto-reconexión activa obligaría a usar `MQTT_EVENT_BEFORE_CONNECT`; con reconexión manual es más simple y determinista. |
| MQTT-4 | Componente oficial + Nativo | esp-tls/mbedTLS: `broker.verification.certificate` (PEM), `crt_bundle_attach = esp_crt_bundle_attach` (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`), verificación de hostname por defecto (`skip_cert_common_name_check = false`). TLS 1.3: `CONFIG_MBEDTLS_SSL_PROTO_TLS1_3`. | S | Cada sesión TLS consume unos 40 KB de heap. Con PSRAM, `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`. Sin reloj válido (TIME-1/2) la validación de certificados falla por fecha. |
| MQTT-5 | Componente oficial | `credentials.authentication.certificate/key`. Para proteger la clave: periférico DS del S3 (`esp_ds`, `CONFIG_ESP_TLS_USE_DS_PERIPHERAL`) con `espressif/esp_secure_cert_mgr` 2.9.3. | M | Aprovisionar claves en eFuse/DS es irreversible y necesita un proceso de fábrica. |
| MQTT-6 | Trabajo propio | Lista de brokers en la configuración. Tras N fallos, `esp_mqtt_set_config()` con el siguiente host y luego `esp_mqtt_client_reconnect()`. | S | Encaja en el gestor de conexión actual (`gw_edge`). |
| MQTT-7 ✅ | Componente oficial + Trabajo propio | Colas FreeRTOS estáticas (`xQueueCreateStatic`) y búfers de comandos en un *pool* fijo. esp-mqtt: `outbox.limit` (bytes), `CONFIG_MQTT_SKIP_PUBLISH_IF_DISCONNECTED`. QoS 0 se escribe directo en el socket, sin outbox (doc. de `esp_mqtt_client_publish`). | S | Un broker lento bloquea `esp_mqtt_client_publish` hasta `network.timeout_ms`. No crece la memoria. lwIP sí asigna pbufs del heap internamente (`MEM_LIBC_MALLOC`). |
| MQTT-8 | Componente oficial | `CONFIG_MQTT_PROTOCOL_5`, `mqtt5_client.c`, API `esp_mqtt5_client_set_*_property`. | S–M | Sparkplug B 3.0 se especifica sobre MQTT 3.1.1 y 5.0. Para *session expiry* y el will hay que revisar la semántica. |

## 4. Sparkplug B (SPB)

| ID | Viabilidad | Mecanismo concreto | Esf. | Riesgos / notas |
|---|---|---|---|---|
| SPB-1 ✅ | Terceros | **nanopb**: no existe componente de Espressif. En el registro solo hay de terceros (`livekit/nanopb` 0.4.9~2, `nikas-belogolov/nanopb`, `surajdev/esp-nanopb`). **`livekit/nanopb` se descartó** porque compila con `PB_BUFFER_ONLY`/`PB_ENABLE_MALLOC` como definiciones *PRIVATE*: el layout de `pb_istream_t` que ve el consumidor difiere del de la librería, un error de ABI latente. Se vendoriza el runtime upstream `nanopb-0.4.9.1` (Zlib). El código se genera con `nanopb_generator` 0.4.9.1 a partir del `sparkplug_b.proto` de eclipse/tahu **sin modificar**, más un `.options`. IDF trae `protobuf-c` (núcleo, para protocomm), una alternativa sin vendorizar pero con `malloc` en el decode. | S | Las opciones de nanopb (`max_size:64` para nombres y strings, `FT_CALLBACK` para `metrics`, `FT_IGNORE`/`skip_message` para Template/DataSet/PropertySet) limitan lo que se puede decodificar. Ampliarlas es parte de SPB-16/18/19. |
| SPB-2 ✅ | Trabajo propio | NDEATH QoS 1 sin retain como will. Desconexión ordenada: publicar NDEATH y luego **`esp_mqtt_client_stop()`**, que envía DISCONNECT de forma síncrona. | S | **Encontrado en la prueba**: `esp_mqtt_client_disconnect()` solo levanta un bit. Si se reinicia enseguida, el broker publica además el will, un **NDEATH duplicado**. Con `stop()` queda uno solo (verificado en QEMU). |
| SPB-3 ✅ | Nativo | NVS (`nvs_get_u8`/`nvs_set_u8` + `nvs_commit`), un incremento por intento. | S | Desgaste: un intento por minuto en el peor caso. NVS rota entradas por páginas, lo que da décadas de vida útil. |
| SPB-4 ✅ | Trabajo propio | Motor `gw_node` (C puro). | S | — |
| SPB-5 ✅ | Trabajo propio | `uint8_t` compartido, NDEATH excluido. | S | — |
| SPB-6 ✅ | Trabajo propio | DBIRTH/DDEATH reconciliados contra el estado en línea del dispositivo. | S | Un DBIRTH de 64 tags pesa unos 4,5 KB, más que el búfer de 2 KB de esp-mqtt. La publicación fragmentada se verificó en QEMU con un DBIRTH de 2684 bytes. |
| SPB-7 ✅ | Trabajo propio | RBE contra el último valor *reportado*, un DDATA por ciclo con alias y sin nombre. | S | — |
| SPB-8 ✅ | Trabajo propio | Mapa de alias fijo (1–27 nodo, 100+n tags), UInt64. | S | Los alias cambian si se reordena la configuración. Sparkplug lo admite, porque cada BIRTH los redeclara. |
| SPB-9 ✅ | Trabajo propio | NCMD `Node Control/Rebirth`. | S | — |
| SPB-10 ✅ | Trabajo propio | DCMD → cola hacia el dueño del bus → escritura → relectura → DDATA. | S | — |
| SPB-11 ✅ | Trabajo propio | NCMD `DO/n` → TCA9554; `Node Control/Reboot` → NDEATH + `esp_restart()`. | S | — |
| SPB-12 ✅ | Trabajo propio | Banda muerta absoluta por tag. | S | — |
| SPB-13 | Trabajo propio | Suscripción a `spBv1.0/STATE/{id}`, payload JSON `{"online":bool,"timestamp":n}` con `cJSON`; no publicar NBIRTH hasta `online:true`. | S–M | En 3.0 el STATE es JSON retenido. Hay que manejar la carrera entre CONNECT, SUBSCRIBE y el retenido, y el timestamp para descartar STATE antiguos. |
| SPB-14 | Nativo + Trabajo propio | Búfer circular en PSRAM (`heap_caps_malloc(MALLOC_CAP_SPIRAM)` una vez al arranque) y volcado opcional a microSD (`esp_vfs_fat_sdmmc_mount`, `SDMMC_HOST_DEFAULT` con `slot.width = 1`, CLK 48, CMD 47, D0 45). `is_historical` ya está en el codec. | L | **GPIO45 es pin de *strapping* (tensión de VDD_SPI)**: una pull-up de la SD en D0 puede forzar 1,8 V y bloquear el arranque si no se fija `VDD_SPI` por eFuse. Hay que verificarlo en el esquemático. Las reglas de reenvío de Sparkplug 3.0 (orden, rebirth, host primario) son delicadas. |
| SPB-15 | Trabajo propio | Métrica escribible → `gw_poller` cambia `poll_ms`. | S | — |
| SPB-16 | Trabajo propio | PropertySet en nanopb (`engUnit`, `engLow`, `engHigh`, `Quality`): quitar `skip_message` y dar `max_count`. El esquema ya lee `units`. | M | Los DBIRTH crecen; el campo `Quality` usa PropertyValue anidado. |
| SPB-17 | Nativo | `esp_get_free_heap_size()`, `esp_timer_get_time()`, `mb_stats_t` (ya existe), `esp_wifi_sta_get_ap_info()` para el RSSI. | S | — |
| SPB-18 | Trabajo propio | Template es recursivo (Metric ↔ Template): con nanopb exige callbacks anidados. | L | Pocos hosts lo usan bien. |
| SPB-19 | Trabajo propio | DataSet, Bytes y File con callbacks de nanopb. File exige troceado y payloads grandes. | M–L | Memoria acotada difícil de garantizar con File. |

## 5. Modbus RTU maestro (MB)

**Decisión: maestro propio, no esp-modbus.** Se revisó `espressif/esp-modbus` 2.1.4 (`modbus/mb_controller/serial/mbc_serial_master.c`, `mb_ports/serial/port_serial.c`):

- Crea dos tareas propias (controlador y puerto serie).
- `mbc_master_get_parameter`/`set_parameter` hacen **`calloc` en cada petición** (líneas 348, 400, 447 y 497). Eso choca con REL-2 en el camino crítico.
- No tiene reintentos.
- Su puerto serie **no** llama a `uart_set_mode`: el modo RS485 lo pone la aplicación.
- El timeout de respuesta por defecto lo fija Kconfig (`FMB_MASTER_TIMEOUT_MS_RESPOND`, rango 150–30000).
- No expone el código de excepción por transacción con el detalle que piden MB-13 y MB-9.

El maestro propio son unas 400 líneas de C puro (`mb_rtu.c`, `mb_master.c`). Además se puede probar en el host (QA-1) y deja el bus con un único dueño (MB-16) sin tareas adicionales.

| ID | Viabilidad | Mecanismo concreto | Esf. | Riesgos / notas |
|---|---|---|---|---|
| MB-1 ✅ | Nativo | `uart_param_config` (baud, `UART_PARITY_*`, `UART_STOP_BITS_1/2`, 8 bits). | S | — |
| MB-2 ✅ | Nativo | `uart_set_pin(UART1, 17, 18, RTS=21, -)` + `uart_set_mode(UART_MODE_RS485_HALF_DUPLEX)`. El driver activa RTS al escribir el FIFO y lo libera en la ISR de `TX_DONE` (verificado en `esp_driver_uart/src/uart.c`). El demo de Waveshare usa el mismo modo con GPIO 21. | S | La liberación de RTS depende de la latencia de la ISR (µs). **QEMU no modela este modo**: `uart_wait_tx_done` hace `assert` por el bit TX_DONE. En la build de emulador se usa UART normal (`CONFIG_GW_RS485_HW_HALF_DUPLEX=n`). |
| MB-3 ✅ | Trabajo propio | FC01–FC04 en `mb_rtu.c`. | S | — |
| MB-4 ✅ | Trabajo propio | FC05, FC06, FC15 y FC16 (FC16 siempre para 32 bits; FC15/FC16 forzables por Kconfig para esclavos sin FC05/FC06). | S | — |
| MB-5 ✅ | Trabajo propio | `gw_conv.c`, ABCD/CDAB. | S | — |
| MB-6 | Trabajo propio | Extensión de `gw_conv`: 64 bits, string, BADC/DCBA, `bit`. | S | Un `double` no representa exactamente un int64 mayor que 2^53: hay que cambiar el tipo interno. |
| MB-7 ✅ | Trabajo propio | `raw*scale+offset` e inversa con redondeo y verificación de rango. | S | — |
| MB-8 ✅ | Nativo + Trabajo propio | `uart_read_bytes` con timeout; `uart_set_rx_timeout(3)` para entregar bytes tras unos 3 caracteres de silencio; t3,5 calculado (1750 µs por encima de 19200) + retardo entre transacciones; reintentos. | S | La granularidad de FreeRTOS es de 1 ms (`CONFIG_FREERTOS_HZ=1000`); por debajo de 2 ms se usa `esp_rom_delay_us`. |
| MB-9 ✅ | Trabajo propio | `gw_devstate.c`. | S | — |
| MB-10 ✅ | Trabajo propio | Planificador de tasa fija sin ráfagas de recuperación. | S | — |
| MB-11 | Trabajo propio | `gw_plan.c`: agrupa contiguos y solapados hasta 125 registros o 2000 bits. El hueco máximo es un Kconfig global (`CONFIG_GW_MB_BLOCK_MAX_GAP`). | S | Implementado (verificado: 60 registros en una sola FC03). El esquema §6 no tiene hueco por dispositivo. |
| MB-12 | Trabajo propio | Clases de escaneo por tag; requiere extender el esquema. | M | — |
| MB-13 | Trabajo propio | El código de excepción ya se decodifica y se registra en el log. Faltan contadores por dispositivo (hoy son globales en `mb_stats_t`). | S | — |
| MB-14 | Trabajo propio | Hoy las escrituras se atienden antes del siguiente sondeo. Con prioridad real, también entre bloques del mismo ciclo. | S | — |
| MB-15 | Trabajo propio (+ Componente oficial parcial) | Servidor TCP 502 con sockets lwIP que traduce MBAP → RTU por la misma cola del dueño del bus. esp-modbus tiene esclavo TCP, pero no la función de pasarela. | M | Hay que arbitrar la latencia entre la encuesta propia y las peticiones de clientes TCP. |
| MB-16 ✅ | Nativo | Una tarea (`rs485_bus`, núcleo 1) y una cola FreeRTOS. | S | — |

## 6. E/S local (IO)

Hechos de la placa verificados con el demo oficial `ESP32-S3-POE-ETH-8DI-8DO-Demo.zip` (Arduino) de files.waveshare.com y con la definición ESPHome de la placa hermana ESP32-S3-ETH-8DI-8RO (mismos pines de DI, I²C y RTC). Detalle y nivel de confianza en [`docs/reference/board-data-to-verify.md`](reference/board-data-to-verify.md).

| ID | Viabilidad | Mecanismo concreto | Esf. | Riesgos / notas |
|---|---|---|---|---|
| IO-1 ✅ | Nativo | `gpio_config` (GPIO 4–11, pull-up) + `esp_timer` periódico de 1 ms (`ESP_TIMER_TASK`) + debounce por integración. | S | La polaridad (activa en bajo) tiene confianza media-alta y es configurable (`CONFIG_GW_DI_ACTIVE_LOW`). Alternativa por interrupción: `gpio_isr_handler_add`. |
| IO-2 ✅ | Trabajo propio (driver mínimo) | TCA9554 en 0x20 con `esp_driver_i2c` (`i2c_master_transmit`). Existe `espressif/esp_io_expander_tca9554` 2.0.3, **descartado**: su `reset()` deja el latch de salida en 0xFF y `esp_io_expander_set_level()` rechaza pines en modo entrada (`ESP_ERR_INVALID_STATE`, verificado en `esp_io_expander.c`). La única secuencia posible es pasar a salida y luego bajar, con un **pulso de las 8 DO a ON** en cada arranque. El driver propio carga el latch antes de configurar la dirección. | S | La lectura de confirmación usa el registro de entrada, que refleja el nivel real del pin. |
| IO-3 | Trabajo propio | Temporizador con la sesión MQTT caída más de N s y modo por salida. Al reiniciar ya se fuerzan las DO a OFF. | S | El TCA9554 mantiene su estado tras un reset del ESP (no tras un corte de alimentación). Con "mantener al reiniciar" habría que leer el latch en lugar de escribirlo. |
| IO-4 | Nativo | Flancos del debounce actual (menos de 25 Hz con 20 ms) o `esp_driver_pcnt` (el S3 tiene 4 unidades PCNT) para frecuencias mayores. Persistencia en NVS periódica o en la RAM del PCF85063. | S–M | Desgaste de flash si se persiste demasiado a menudo. |
| IO-5 ✅ | Componente oficial | `espressif/led_strip` 3.1.1 (RMT, `LED_MODEL_WS2812`, `LED_STRIP_COLOR_COMPONENT_FMT_RGB`), GPIO 38. | S | El orden RGB (no GRB) se dedujo del demo (`neopixelWrite(pin, g, r, b)`) y de ESPHome (`rgb_order: RGB`): confianza media. |
| IO-6 | Nativo | `esp_driver_ledc` a unos 1–4 kHz en GPIO 46 (el demo usa LEDC a 1 kHz, lo que sugiere un buzzer **pasivo**). | S | **GPIO46 es pin de *strapping*** (debe estar bajo al arrancar). No conducirlo antes de `app_main`. |
| IO-7 | Trabajo propio | Tabla de reglas DI→DO evaluada en el callback del debounce. Sin red funciona igual. | S–M | Definir prioridad frente a los NCMD. |

## 7. Configuración (CFG)

| ID | Viabilidad | Mecanismo concreto | Esf. | Riesgos / notas |
|---|---|---|---|---|
| CFG-1 ✅ | Componente oficial + Trabajo propio | `espressif/cjson` (desde 6.0 fuera del núcleo) + validador propio con rutas JSON en los errores. | S | cJSON asigna memoria al parsear, solo en el arranque. |
| CFG-2 ✅ | Terceros + Nativo | `joltwallet/littlefs` 1.23.1 (`esp_vfs_littlefs_register`, `littlefs_create_partition_image(... FLASH_IN_PROJECT)`) + `EMBED_TXTFILES` para el JSON por defecto. | S | `joltwallet` no es de Espressif, aunque es el estándar de facto. Alternativa nativa: `spiffs` o `fatfs` + `wear_levelling`. |
| CFG-3 | Nativo | `esp_http_server` (o `esp_https_server`) con estáticos en LittleFS. Basic auth: no hay helper en el núcleo; el ejemplo `protocols/http_server/simple` muestra cómo hacerlo. | M–L | El grueso del trabajo es la UI. Sin TLS, las credenciales viajan en claro. |
| CFG-4 | Nativo + Trabajo propio | Handlers REST con cJSON. Validar con `gw_config_parse`, escribir de forma atómica (archivo temporal + `rename`) y reiniciar. | S–M | — |
| CFG-5 | Nativo | Sondeo de GPIO0 durante más de 10 s → `esp_littlefs_format("storage")` + `nvs_flash_erase()`. | S | GPIO0 es *strapping*: no bloquear el arranque con el botón pulsado. |
| CFG-6 | Nativo | `components/console` (`esp_console_new_repl_uart`, argtable3, linenoise). | S–M | Comparte UART0 con los logs. |
| CFG-7 | Nativo | Cifrado de NVS: `CONFIG_NVS_ENCRYPTION` con `nvs_sec_provider` en modo HMAC (`CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC`, clave HMAC en eFuse del S3, sin exigir cifrado de flash). | M | Hoy `mqtt.password` está en claro en el JSON de LittleFS. Habría que mover los secretos a NVS cifrada o cifrar la partición. **LittleFS sobre partición cifrada**: el modo *blockdev* de `joltwallet/littlefs` rechaza dispositivos cifrados (`esp_littlefs.c`, "BDL encrypted block devices are not supported"); el modo por partición sí lo admite según su README. Hay que verificarlo. |

## 8. Firmware (FW)

| ID | Viabilidad | Mecanismo concreto | Esf. | Riesgos / notas |
|---|---|---|---|---|
| FW-1 | Nativo | `esp_https_ota` (`esp_https_ota_begin/perform/finish`), particiones `ota_0`/`ota_1` (ya reservadas en `idf/partitions.csv`), `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` + `esp_ota_mark_app_valid_cancel_rollback()` tras un NBIRTH exitoso. Disparo por NCMD (métrica String con la URL) o por web. | M | Hay que definir el criterio de "salud" y el tiempo antes del rollback. |
| FW-2 | Nativo | `CONFIG_SECURE_BOOT_V2_ENABLED` (en el S3 solo RSA-3072 PSS: `soc_caps.h` define `SOC_SECURE_BOOT_V2_RSA` y no la variante ECC), firma con `espsecure.py`/`idf.py`, compatible con OTA. | M | Irreversible (eFuse). Sin una gestión de claves seria es mejor no habilitarlo. Hay que deshabilitar JTAG y el modo descarga en producción. |
| FW-3 | Nativo | `CONFIG_SECURE_FLASH_ENC_ENABLED` (AES-XTS en el S3), modo *release*. | M | Irreversible; ver la nota de LittleFS en CFG-7. Las particiones NVS no se cifran con flash encryption; usan su propio esquema de NVS encryption. |
| FW-4 ✅ | Nativo | `PROJECT_VER` = `0.1.0+g<hash>[.dirty]` calculado en `CMakeLists.txt` → `esp_app_get_description()->version`. | S | — |

## 9. Robustez (REL)

| ID | Viabilidad | Mecanismo concreto | Esf. | Riesgos / notas |
|---|---|---|---|---|
| REL-1 ✅ | Nativo | TWDT (`CONFIG_ESP_TASK_WDT_*`, 15 s, *panic*) + `esp_task_wdt_add/reset` en las tareas críticas. También vigila las tareas IDLE de los dos núcleos. | S | Las llamadas bloqueantes deben quedar acotadas por debajo del TWDT. Modbus alimenta el TWDT en cada intento; esp-mqtt usa `network.timeout_ms = 5000`. |
| REL-2 ✅ | Nativo + Trabajo propio | Arrays estáticos (16 dispositivos, 64 tags, 256 en total), colas y pilas con `*CreateStatic`. | S | El código propio no asigna memoria en el camino crítico. **lwIP y esp-mqtt sí lo hacen internamente** (pbufs y outbox para QoS > 0): acotado, pero no estático. |
| REL-3 ✅ | Nativo | `esp_log` por niveles en UART0. | S | — |
| REL-4 | Trabajo propio | `esp_log_set_vprintf()` + socket UDP (RFC 5424) o publicación en MQTT. No hay cliente syslog en IDF ni en el registro. | S | Hay que evitar la recursión (logs del propio envío). |
| REL-5 | Nativo | `esp_reset_reason()` (ya se registra en el log al arrancar) → métrica String en NBIRTH. Core dump a flash (`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`, partición `coredump` incluida). | S | — |
| REL-6 | Nativo | Medido en QEMU: **NBIRTH unos 2,1 s después de arrancar el emulador** (DHCP + SNTP + MQTT). | S | En hardware hay que sumar la negociación del PHY del W5500 (1–3 s). Con TLS, unos 0,5–1 s más. |

## 10. Calidad (QA)

| ID | Viabilidad | Mecanismo concreto | Esf. | Riesgos / notas |
|---|---|---|---|---|
| QA-1 ✅ | Trabajo propio | Lógica en `components/gw_core` (C puro, sin IDF) + CMake/CTest + Unity v2.6.1 en el host: 9 ejecutables, 54 casos. Alternativa nativa: target `linux` de IDF. | S | — |
| QA-2 ✅ | Nativo | `idf.py set-target esp32s3 build`: 0 warnings; `-Wall -Wextra -Werror` en todos los componentes propios. | S | `idf.py` no expone `-j`: ninja usa por defecto nproc+2. |
| QA-3 | Nativo (QEMU) + Trabajo propio | QEMU de Espressif `esp_develop_9.2.2` (`idf_tools.py install qemu-xtensa`, necesita `libslirp0`). Máquina `esp32s3` con NIC `open_eth` (driver nativo `esp_eth_mac_new_openeth`, `CONFIG_ETH_USE_OPENETH`) y UART por chardev. Contra mosquitto y pymodbus: 22/22 comprobaciones. | M | El emulador **no** modela el W5500, el TCA9554, el GPIO, el RMT, la PSRAM octal (QEMU hace *segfault* con `is_octal`), las lecturas QIO de la app (datos desplazados 2 bytes) ni el modo RS485 de la UART. La build de QEMU usa un overlay (`sdkconfig.qemu`) con simulación de E/S. Lo que el emulador no cubre exige prueba en hardware. |
| QA-4 | Nativo | Imagen Docker `espressif/idf:v6.1` en GitHub Actions: build, tests de host y el job QEMU (`tools/e2e/run_e2e.sh`). | S | — |

## 11. Resumen: mayores brechas y riesgos de ESP-IDF para este producto

1. **Dependencias fuera del núcleo y de terceros.**
   - Desde IDF 6.0, esp-mqtt, cJSON, el driver W5500 y el aprovisionamiento son componentes del registro con versiones propias. Hay que fijarlas (`==x.y.z` + `dependencies.lock`) y seguirlas.
   - nanopb y LittleFS no son de Espressif, y el paquete nanopb del registro tiene un defecto de ABI.
   - El componente oficial del TCA9554 no permite un arranque sin pulsos en las salidas.

   El ecosistema es amplio, pero cada pieza exige revisión.
2. **Las pruebas en emulador no cubren el hardware crítico.**
   - QEMU valida la lógica, la pila TCP/IP, MQTT y Sparkplug de punta a punta con el firmware real.
   - No valida el W5500, el modo RS485 por hardware (temporización de RTS/DE), el TCA9554, la polaridad real de las DI, la PSRAM octal ni el LED.
   - Antes de producción hace falta una campaña en placa: tiempos de giro del bus a 115200, ruido y aislamiento, y DO con cargas inductivas.
3. **Seguridad y memoria cuando se agreguen los P1.**
   - TLS mutuo + Wi-Fi + web + store & forward compiten por la RAM interna (hoy un 44 % de DIRAM usada sin TLS). Hay que mover búferes a PSRAM.
   - Secure boot y flash encryption son irreversibles.
   - El cifrado de la configuración en LittleFS no está resuelto de forma limpia (CFG-7).
   - Los pines de *strapping* compartidos con periféricos de la placa (GPIO45 de la SD, GPIO46 del buzzer, GPIO0 del BOOT) imponen restricciones de diseño a SPB-14, IO-6 y CFG-5.
