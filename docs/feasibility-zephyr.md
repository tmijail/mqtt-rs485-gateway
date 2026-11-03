# Viabilidad con Zephyr RTOS: gateway RS485 → Sparkplug B (ESP32-S3-POE-ETH-8DI-8DO)

Estudio de viabilidad de **todos** los requisitos de `docs/SPEC.md` §4 sobre Zephyr. No se apoya en la
memoria: cada mecanismo se comprobó en el árbol instalado y la implementación del MVP en `zephyr/` lo
ejercita cuando corresponde.

- **Árbol de referencia:** Zephyr **v4.5.0** (última release estable a 2026-11-03), `hal_espressif`
  `cc81db0`, `mbedtls` 4.1.1, `nanopb`, `littlefs`, `mcuboot`, `picolibc`, `zcbor`. Workspace west
  mínimo con `name-allowlist`, en `/home/user/sdk/zephyr/ws`. Toolchain: Zephyr SDK 1.0.1, solo
  `xtensa-espressif_esp32s3_zephyr-elf`.
- Las rutas son relativas a `zephyr/` (el repositorio de Zephyr) salvo que empiecen por `modules/` o
  `bootloader/`.
- **Viabilidad:** `Nativo` (Zephyr upstream ya lo trae), `Trabajo propio` (se construye con primitivas
  de Zephyr y código nuestro), `Terceros` (hace falta un módulo o biblioteca externa) o
  `No viable / muy riesgoso`.
- **Esfuerzo:** S (≤ 2 días), M (≤ 2 semanas), L (> 2 semanas), siempre sobre la base del MVP ya hecho.
- La columna **MVP** indica si el requisito ya está implementado en `zephyr/` (✅), parcialmente (◐) o
  no (—). El detalle está en `zephyr/README.md`.

## Hallazgos que condicionan el diseño (verificados en el código fuente)

1. **W5500 en MACRAW con la pila IP nativa: sí.** `drivers/ethernet/wiznet/eth_wiznet.c` es un núcleo
   MACRAW compartido (`s0_mr_macraw`) con el registro de L2 Ethernet de Zephyr. El hilo de monitor
   (`CONFIG_ETH_WIZNET_MONITOR_PERIOD`, 500 ms) hace `net_if_carrier_on/off` cuando cambia el enlace.
   Por defecto queda activado el **filtro MAC (MFEN)**, que descarta el multicast. mDNS e IPv6 (ND)
   necesitan `CONFIG_NET_PROMISCUOUS_MODE`, que el driver implementa limpiando ese bit.
2. **El RS485 por hardware de la UART no sirve con el subsistema Modbus.** `drivers/serial/uart_esp32.c`
   admite `hw-rs485-hd-mode` (`UART_CFG_FLOW_CTRL_RS485` → `UART_MODE_RS485_HALF_DUPLEX`). Pero:
   (a) el binding `dts/bindings/serial/espressif,esp32-uart.yaml` dice que en ese modo se activa la
   señal **DTR**, no RTS; (b) el driver no conmuta `sw_rts` en la ISR, como sí hace ESP-IDF; y
   (c) `subsys/modbus/modbus_serial.c::configure_uart()` fija `flow_ctrl = UART_CFG_FLOW_CTRL_NONE`,
   así que pisa el modo RS485 al inicializar el cliente. **Conclusión:** se usa `de-gpios` del
   subsistema Modbus (GPIO21 por software, conmutado en la ISR de TX-complete), que es fiable. El modo
   por hardware queda como trabajo propio no verificado.
3. **El cliente MQTT de Zephyr no reconecta** (`subsys/net/lib/mqtt/`: no hay reconexión ni backoff).
   La sesión, el backoff y el LWT los gestiona la aplicación (`zephyr/src/mqtt_link.c`).
4. **Los blobs de Wi-Fi no se pudieron descargar en este entorno.** `west blobs fetch hal_espressif`
   falló con HTTP 403 en los 73 blobs de `github.com/espressif/esp32-wifi-lib`, `esp-phy-lib` y
   `esp-coex-lib`: el proxy no permite esos repositorios. El MVP (solo Ethernet) compila sin blobs.
   Todo lo que usa Wi-Fi o BLE **no se pudo compilar aquí**.
5. **Secure boot y cifrado de flash** existen solo a través del **port Espressif de MCUboot**
   (`bootloader/mcuboot/boot/espressif/hal/src/secure_boot.c`, `flash_encrypt.c`, y
   `port/esp32s3/bootloader.conf` con `CONFIG_SECURE_BOOT_V2_ENABLED` comentado). En Zephyr,
   `ESP_FLASH_ENCRYPTION` (`soc/espressif/common/Kconfig.flash`) es "compatibilidad" y exige
   `write-block-size = 32`. No forman parte del flujo sysbuild habitual.

## 4.1 Red (NET)

| ID | MVP | Viabilidad | Mecanismo concreto | Esf. | Riesgos o notas |
|---|---|---|---|---|---|
| NET-1 | ✅ | Nativo | `drivers/ethernet/wiznet/eth_w5500.c` + `eth_wiznet.c` (MACRAW), binding `wiznet,w5500` (`int-gpios`, `reset-gpios`), `CONFIG_ETH_W5500`. Enlace: hilo monitor → `net_if_carrier_on/off`. Disponibilidad: `CONFIG_NET_CONNECTION_MANAGER` (`NET_EVENT_L4_CONNECTED/DISCONNECTED`) | S | Sin hardware no se midió la velocidad SPI real; se configuró a 20 MHz (el W5500 admite 33 MHz y hay margen). La MAC sale del eFuse vía `hwinfo_get_device_id` (+3, convenio de IDF) con `NET_REQUEST_ETHERNET_SET_MAC_ADDRESS` (`CONFIG_NET_L2_ETHERNET_MGMT`). |
| NET-2 | ✅ | Nativo | `CONFIG_NET_DHCPV4` (`net_dhcpv4_start`), estática con `net_if_ipv4_addr_add`, `net_if_ipv4_set_netmask_by_addr` y `net_if_ipv4_set_gw` (`include/zephyr/net/net_if.h`) | S | DHCP renueva solo al volver el carrier. |
| NET-3 | ✅ | Nativo | `CONFIG_DNS_RESOLVER`, `zsock_getaddrinfo`; DNS por DHCP (`CONFIG_NET_DHCPV4_OPTION_DNS_ADDRESS`, por defecto y) o estático con `dns_resolve_reconfigure(..., DNS_SOURCE_MANUAL)` | S | — |
| NET-4 | — | Nativo + Trabajo propio | `drivers/wifi/esp32/src/esp_wifi_drv.c` (STA), `wifi_mgmt`; varias interfaces a la vez con `conn_mgr`. La conmutación por prioridad (interfaz por defecto o ruta) es nuestra | M | **Riesgo alto en este entorno:** blobs no descargables (hallazgo 4). La coexistencia Wi-Fi + W5500 + PSRAM en RAM interna está sin medir. ESPHome documenta que no los usa a la vez, pero es una limitación suya, no de Zephyr. |
| NET-5 | — | Nativo + Trabajo propio | `esp32_wifi_ap_enable` (`esp_wifi_drv.c:1867`), `CONFIG_NET_DHCPV4_SERVER`; botón BOOT con `gpio-keys` + `CONFIG_INPUT` | M | Mismo riesgo de blobs. El portal de puesta en marcha depende de CFG-3. |
| NET-6 | — | Nativo | `CONFIG_NET_HOSTNAME_DYNAMIC` (`net_hostname_set`), `subsys/net/lib/dns/mdns_responder.c`, `CONFIG_DNS_SD` para `_http._tcp` | S | Con W5500 hace falta `CONFIG_NET_PROMISCUOUS_MODE` (hallazgo 1), lo que añade carga de CPU por el tráfico ajeno. |
| NET-7 | — | Nativo | `CONFIG_NET_IPV6` (pila nativa), SLAAC, DHCPv6 (`subsys/net/lib/dhcpv6`) | S | Mismo filtro MAC: ND/RA son multicast. Sin verificar en hardware. |

## 4.2 Tiempo (TIME)

| ID | MVP | Viabilidad | Mecanismo concreto | Esf. | Riesgos o notas |
|---|---|---|---|---|---|
| TIME-1 | ✅ | Nativo | `subsys/net/lib/sntp` (`sntp_simple`), `sys_clock_settime(SYS_CLOCK_REALTIME)` (`lib/os/clock.c`) | S | Antes de la primera sincronización el reloj cuenta desde 1970 al arrancar. El MVP espera hasta 5 s por SNTP antes de la primera conexión. |
| TIME-2 | — | Nativo | `drivers/rtc/rtc_pcf85063a.c` (`nxp,pcf85063a`), API `rtc_get_time/rtc_set_time`; el nodo ya está en la DTS de la placa (`status = "disabled"`) | S | Chip confirmado (PCF85063ATL, 0x51) por el wiki y la demo de Waveshare. |
| TIME-3 | — | Trabajo propio | Una métrica más en `spb_node.c` a partir de `gw_time_synced()` | S | — |

## 4.3 MQTT (MQTT)

| ID | MVP | Viabilidad | Mecanismo concreto | Esf. | Riesgos o notas |
|---|---|---|---|---|---|
| MQTT-1 | ✅ | Nativo | `subsys/net/lib/mqtt` (`CONFIG_MQTT_LIB`, `MQTT_VERSION_3_1_1`) | S | Biblioteca de bajo nivel: el bucle `poll/mqtt_input/mqtt_live` lo escribe la aplicación. |
| MQTT-2 | ✅ | Trabajo propio | No hay reconexión upstream (hallazgo 3). `zephyr/src/backoff.h` + `mqtt_link.c` | S | — |
| MQTT-3 | ✅ | Nativo | `mqtt_client.will_topic/will_message`, QoS en `mqtt_topic.qos`, `will_retain` | S | — |
| MQTT-4 | — | Nativo | `CONFIG_MQTT_LIB_TLS` → `mqtt_transport_socket_tls.c`, sockets TLS (`subsys/net/lib/sockets/sockets_tls.c`), `tls_credential_add(TLS_CREDENTIAL_CA_CERTIFICATE)`, `ZSOCK_TLS_HOSTNAME` para verificar el nombre; mbedTLS 4.1.1 (`modules/crypto/mbedtls`) | M | RAM: un handshake TLS 1.2/1.3 con mbedTLS pide unos 40–60 KB (estimación, sin medir). Conviene PSRAM o ajustar `MBEDTLS_SSL_*_CONTENT_LEN`. mbedTLS 4 es nuevo (TF-PSA-Crypto) y puede cambiar Kconfig. |
| MQTT-5 | — | Nativo | Igual que MQTT-4 con `TLS_CREDENTIAL_PUBLIC_CERTIFICATE` y `TLS_CREDENTIAL_PRIVATE_KEY` | S | Guardar la clave depende de CFG-7. |
| MQTT-6 | — | Trabajo propio | Lista de brokers en el JSON e iteración en `connect_broker()` | S | Cambia el esquema común (§6), que habría que coordinar con IDF. |
| MQTT-7 | ✅ | Trabajo propio | Diseño sin cola de publicación: estado coalescido en `model.c` (bits sucios atómicos) + `k_msgq` acotada para DCMD; buffers estáticos de MQTT | S | Probado por diseño. Un broker lento solo retrasa: `SO_SNDTIMEO` de 10 s (`CONFIG_NET_CONTEXT_SNDTIMEO`). |
| MQTT-8 | — | Nativo | `CONFIG_MQTT_VERSION_5_0` (`subsys/net/lib/mqtt/Kconfig:33`) | S | Sparkplug 3.0 se especifica sobre 3.1.1/5.0. Sin probar. |

## 4.4 Sparkplug B 3.0 (SPB)

No hay biblioteca Sparkplug en Zephyr. Todo el protocolo es trabajo propio sobre nanopb (módulo
`modules/lib/nanopb`, integración `modules/nanopb/nanopb.cmake`, función `zephyr_nanopb_sources`) con
el `sparkplug_b.proto` oficial de Eclipse Tahu (copiado en `zephyr/proto/`, rama `master`, idéntico a
`develop` a 2026-11-03). El proto es recursivo (Template ↔ Metric, PropertySet ↔ PropertyValue):
`zephyr/proto/sparkplug_b.options` omite esos mensajes (`skip_message`) e ignora los campos que los
usan, de modo que todas las estructuras tienen tamaño fijo.

| ID | MVP | Viabilidad | Mecanismo concreto | Esf. | Riesgos o notas |
|---|---|---|---|---|---|
| SPB-1 | ✅ | Terceros (nanopb, en el manifiesto de Zephyr) | `zephyr_nanopb_sources(app proto/sparkplug_b.proto)`; `src/sparkplug.c` codifica y decodifica las métricas con callbacks (`pb_encode_submessage`) | S | Verificado decodificando con Python `protobuf` y el proto oficial. |
| SPB-2 | ✅ | Trabajo propio | LWT = NDEATH (QoS 1, sin retain); NDEATH explícito en NCMD Reboot | S | El único cierre ordenado del MVP es el reinicio. |
| SPB-3 | ✅ | Nativo | `subsys/settings` + backend ZMS (`CONFIG_SETTINGS_ZMS`, `subsys/fs/zms`) | S | Se escribe en flash en cada intento de conexión. ZMS reparte el desgaste; a un intento por segundo se estiman más de 10 años en 256 KB (cálculo, no medido). |
| SPB-4…SPB-9 | ✅ | Trabajo propio | `src/spb_node.c` | M | Hecho. |
| SPB-10 | ✅ | Trabajo propio | `spb_node.c` → `k_msgq` → `mb_poller.c` (FC05/06/16) con relectura | S | — |
| SPB-11 | ✅ | Trabajo propio | `local_io.c` (`gpio_pin_set_dt` sobre el TCA9554), `sys_reboot` | S | — |
| SPB-12 | ✅ | Trabajo propio | `model_rbe_exceeds()` | S | — |
| SPB-13 | — | Trabajo propio | Suscripción a `spBv1.0/STATE/{id}` y JSON con `lib/utils/json.c`; condicionar `spb_node_session_start()` | S | Hoy solo se avisa en el log de que `primary_host_id` se ignora. |
| SPB-14 | — | Nativo + Trabajo propio | PSRAM: `CONFIG_ESP_SPIRAM` + `SPIRAM_MODE_OCT` (`soc/espressif/common/Kconfig.spiram`), `sys_heap` o `ring_buf` en PSRAM. microSD: `drivers/sdhc/sdhc_esp32.c`, disco `CONFIG_SDMMC_SUBSYS`, FAT (módulo `fatfs`, no incluido en el workspace) o LittleFS | L | Cambia el modelo "estado coalescido" por un historial con marcas de tiempo, y exige `is_historical`, orden y reenvío controlado. La PSRAM octal a 80 MHz y la caché en Zephyr no se probaron aquí. |
| SPB-15 | — | Trabajo propio | Métrica escribible y `poll_ms` dinámico en `mb_poller.c` | S | — |
| SPB-16 | — | Trabajo propio | Habilitar `PropertySet` en `.options` con `FT_CALLBACK` (no estático, por la recursión) | M | La calidad 0/192 exige seguir el estado por tag (ya existe `valid`). |
| SPB-17 | — | Nativo + Trabajo propio | Contadores ya presentes en `gw_dev_state`; heap: `sys_heap_runtime_stats_get`; RSSI: `NET_REQUEST_WIFI_IFACE_STATUS` | S | El RSSI depende de NET-4. |
| SPB-18 | — | Trabajo propio | Template en nanopb con callbacks (recursivo) | L | nanopb con recursión exige callbacks, y el código es delicado. |
| SPB-19 | — | Trabajo propio | DataSet, Bytes y File con `FT_CALLBACK` | M | — |

## 4.5 Modbus RTU maestro (MB)

| ID | MVP | Viabilidad | Mecanismo concreto | Esf. | Riesgos o notas |
|---|---|---|---|---|---|
| MB-1 | ✅ | Nativo | `subsys/modbus` (`CONFIG_MODBUS_ROLE_CLIENT`, `modbus_init_client`), `struct modbus_serial_param` (baud, paridad, stop). Stop bits libres con `CONFIG_MODBUS_NONCOMPLIANT_SERIAL_MODE` | S | Sin esa opción el subsistema fuerza 2 bits de stop con paridad none. |
| MB-2 | ✅ (alternativa GPIO) | Nativo (GPIO) / Trabajo propio (HW) | `de-gpios` en `zephyr,modbus-serial` (`dts/bindings/misc/zephyr,modbus-serial.yaml`). El subsistema activa DE en `modbus_serial_tx_on` y lo libera en la ISR al ver `uart_irq_tx_complete` | S (GPIO) / M (HW) | El modo HW no se puede usar tal cual (hallazgo 2). La latencia de liberación de DE es la de la ISR (µs). |
| MB-3 | ✅ | Nativo | `modbus_read_coils/dinputs/holding_regs/input_regs` (`include/zephyr/modbus/modbus.h`) | S | — |
| MB-4 | ◐ | Nativo | `modbus_write_coil` (FC05), `modbus_write_holding_reg` (FC06), `modbus_write_coils` (FC15), `modbus_write_holding_regs` (FC16) | S | El MVP no emite FC15 (cada coil va con FC05). |
| MB-5 | ✅ | Trabajo propio | `src/convert.c` | S | — |
| MB-6 | — | Trabajo propio | Ampliar `convert.c` (64 bits, string, BADC/DCBA, bit) | S | `bit` implica escritura leer-modificar-escribir. |
| MB-7 | ✅ | Trabajo propio | `convert.c` | S | — |
| MB-8 | ✅ | Nativo + Trabajo propio | Timeout: `modbus_iface_param.rx_timeout`. Silencio de 3,5 caracteres: temporizador RTU del subsistema (`cfg->rtu_timeout`, fijo por encima de 38400 baud, como pide la norma). Reintentos y retardo entre transacciones: `mb_poller.c` | S | El subsistema registra un WRN por cada timeout (`modbus_core.c:172`), así que un esclavo apagado llena el log. Sería un parche upstream o filtrado por nivel. |
| MB-9 | ✅ | Trabajo propio | `mb_poller.c::poll_device` | S | — |
| MB-10 | ✅ | Trabajo propio | Planificador por `next_due` en `mb_poller.c` | S | — |
| MB-11 | — | Trabajo propio | Agrupar tags por área y dirección antes de encuestar | M | Hay que mapear la respuesta del bloque a cada tag y manejar las excepciones del bloque. |
| MB-12 | — | Trabajo propio | `poll_ms` por tag | S | — |
| MB-13 | ◐ | Nativo + Trabajo propio | El cliente devuelve el código de excepción positivo (`modbus_client.c:46-48`). Contadores en `gw_dev_state` (las excepciones cuentan en `error_count`, mezcladas con CRC) | S | Faltan contadores por código y su publicación. |
| MB-14 | ◐ | Trabajo propio | `serve_writes()` antes de cada transacción de lectura | S | Hecho. No hay prioridad entre escrituras. |
| MB-15 | — | Nativo + Trabajo propio | Modo `MODBUS_MODE_RAW` + `modbus_raw_*`; ejemplo `samples/subsys/modbus/tcp_gateway` | M | Hay que serializar con la misma tarea dueña del bus (MB-16). |
| MB-16 | ✅ | Trabajo propio | Un único hilo `mb_poller` hace todas las transacciones | S | — |

## 4.6 E/S local (IO)

| ID | MVP | Viabilidad | Mecanismo concreto | Esf. | Riesgos o notas |
|---|---|---|---|---|---|
| IO-1 | ✅ | Nativo + Trabajo propio | `gpio_pin_get_dt` sobre `/zephyr,user/di-gpios`, `k_timer` de 1 ms, `src/debounce.c` | S | Activas a nivel bajo: confianza media-alta (ver README). `gpio-keys` + `CONFIG_INPUT` no sirve porque su debounce es fijo en la DTS y el spec lo pide configurable. |
| IO-2 | ✅ | Nativo | `drivers/gpio/gpio_tca9554.c` (`ti,tca9554`, nuevo en 4.5); alternativa `gpio_pca_series.c` (`nxp,pca9554`) | S | Salidas activas a nivel bajo: confianza media. |
| IO-3 | — | Trabajo propio | Temporizador por pérdida de MQTT; estado al arrancar (hoy OFF siempre) | S | — |
| IO-4 | — | Nativo + Trabajo propio | `drivers/sensor/espressif/pcnt_esp32` (PCNT por hardware) o conteo en el muestreo de 1 kHz; persistencia con settings | M | Con muestreo a 1 kHz el máximo es unos 500 Hz; por encima hace falta PCNT. |
| IO-5 | ✅ | Nativo | `drivers/led_strip/ws2812_pulse_io.c` (`worldsemi,ws2812-pulse-io`) sobre `drivers/pulse_io/pulse_io_esp32_rmt*.c`. Alternativas: `ws2812-spi`, `ws2812-i2s` | S | Sin hardware: el orden de color RGB sale de la demo y no se ha visto en una placa. |
| IO-6 | — | Nativo | `drivers/pwm/pwm_led_esp32.c` (LEDC), nodo `ledc0` ya descrito en la DTS (deshabilitado) | S | Buzzer pasivo, PWM de 1 kHz en la demo. |
| IO-7 | — | Trabajo propio | Tabla DI→DO en el hilo `local_io` | S | — |

## 4.7 Configuración (CFG)

| ID | MVP | Viabilidad | Mecanismo concreto | Esf. | Riesgos o notas |
|---|---|---|---|---|---|
| CFG-1 | ✅ | Trabajo propio | Lector JSON propio (`src/json_reader.c`) + validador (`src/config.c`) | M | Se descartó `lib/utils/json.c` de Zephyr para el documento completo. Necesita descriptores con arrays de tamaño máximo anidados (16 × 64 tags en RAM solo para parsear), no da posición del error y su escaneo `arr_data_parse()` no contempla comillas escapadas. |
| CFG-2 | ✅ | Nativo | LittleFS (`subsys/fs/littlefs_fs.c`, módulo `littlefs`) montado por `zephyr,fstab,littlefs` con `automount`; JSON por defecto embebido con `generate_inc_file_for_target` | S | El MVP no trae herramienta para escribir el archivo en el equipo. Opciones nativas: `mcumgr` `fs_mgmt` (`subsys/mgmt/mcumgr/grp/fs_mgmt`) o una imagen LittleFS generada en el build. |
| CFG-3 | — | Nativo + Trabajo propio | `subsys/net/lib/http/http_server_core.c` (`CONFIG_HTTP_SERVER`, recursos estáticos y dinámicos, WebSocket) | L | **No hay autenticación en el servidor HTTP** (no aparece `Authorization` en `subsys/net/lib/http`). Basic Auth propia con `CONFIG_HTTP_SERVER_CAPTURE_HEADERS`. La interfaz (HTML/JS) es lo más costoso. |
| CFG-4 | — | Nativo + Trabajo propio | Mismo servidor + `gw_config_parse` (ya escrito) | M | — |
| CFG-5 | — | Nativo + Trabajo propio | `gpio-keys` (botón BOOT ya en la DTS, alias `sw0`) + `CONFIG_INPUT` con `zephyr,input-longpress` (`subsys/input/input_longpress.c`), borrado de `/lfs/config.json` y settings | S | — |
| CFG-6 | ◐ | Nativo | `subsys/shell` (activo; comando `gw status`) | S | Falta ver y cambiar la configuración. Con `CONFIG_FILE_SYSTEM_SHELL` se puede leer el archivo. |
| CFG-7 | — | Nativo (parcial) | `subsys/secure_storage` (PSA ITS, transform AEAD, store ZMS o settings) | M | La clave de la AEAD por defecto deriva de un ID de dispositivo, que no es secreto. Sin cifrado de flash (FW-3) la protección es limitada. |

## 4.8 Firmware (FW)

| ID | MVP | Viabilidad | Mecanismo concreto | Esf. | Riesgos o notas |
|---|---|---|---|---|---|
| FW-1 | — | Nativo + Trabajo propio | MCUboot vía sysbuild (`SB_CONFIG_BOOTLOADER_MCUBOOT`; la partición `boot_partition` y los `slot0/slot1` de 6 MB ya están en la DTS de la placa), `subsys/dfu` (`flash_img`, `boot_request_upgrade`, `boot_write_img_confirmed` para el rollback), descarga con `subsys/net/lib/http/http_client.c` + TLS; o `mcumgr` `img_mgmt` | M | El MVP arranca con `ESP_SIMPLE_BOOT` (sin MCUboot). El flujo OTA en ESP32-S3 con MCUboot no se probó aquí. |
| FW-2 | — | No viable / muy riesgoso (vía Zephyr) / Terceros (port Espressif de MCUboot) | `bootloader/mcuboot/boot/espressif/hal/src/secure_boot.c`, `port/esp32s3/bootloader.conf` (`CONFIG_SECURE_BOOT_V2_ENABLED`, comentado) | L | Las firmas de imagen de MCUboot (ECDSA o RSA) son nativas. El **secure boot del ROM** (eFuses, irreversible) solo se configura con el port Espressif de MCUboot fuera del flujo sysbuild normal. Un error deja la placa inutilizable. |
| FW-3 | — | No viable / muy riesgoso (vía Zephyr) / Terceros | `soc/espressif/common/Kconfig.flash` (`ESP_FLASH_ENCRYPTION`: "compatibilidad", exige `!ESP_SIMPLE_BOOT` y `write-block-size = 32`) + `flash_encrypt.c` del port de MCUboot | L | Afecta a LittleFS y ZMS (escrituras de 32 B). Sin probar. |
| FW-4 | ✅ | Nativo | `VERSION` de la aplicación → `zephyr/app_version.h` (`APP_VERSION_STRING`, `APP_BUILD_VERSION` = `git describe`) | S | — |

## 4.9 Robustez y observabilidad (REL)

| ID | MVP | Viabilidad | Mecanismo concreto | Esf. | Riesgos o notas |
|---|---|---|---|---|---|
| REL-1 | ✅ | Nativo | `subsys/task_wdt` con `wdt0` (`espressif,esp32-watchdog`) como respaldo (`CONFIG_TASK_WDT_HW_FALLBACK`) | S | Canales: Modbus, MQTT, red, E/S. Sin hardware no se comprobó que el reset dispare. |
| REL-2 | ✅ | Trabajo propio | Todo estático y acotado por Kconfig (`CONFIG_GW_MAX_*`). Sin `malloc` en encuesta ni publicación | S | `zsock_getaddrinfo` puede usar el heap del sistema en cada conexión, fuera del camino crítico. |
| REL-3 | ✅ | Nativo | `subsys/logging`, backend de shell en la consola USB-Serial/JTAG | S | — |
| REL-4 | — | Nativo / Trabajo propio | Syslog: `CONFIG_LOG_BACKEND_NET` (`subsys/logging/backends/log_backend_net.c`). Por MQTT: backend propio | S/M | — |
| REL-5 | ◐ | Nativo | `hwinfo_get_reset_cause` (`drivers/hwinfo/hwinfo_esp32.c`); hoy solo se registra en el log | S | Falta publicarlo en NBIRTH. |
| REL-6 | ◐ | Nativo | — | S | En `native_sim` llegó al NBIRTH en 0,01 s. En hardware dependerá de DHCP y del enlace (unos 2–3 s, estimado, sin medir). Un servidor SNTP caído añade hasta 5 s de espera. |

## 4.10 Calidad (QA)

| ID | MVP | Viabilidad | Mecanismo concreto | Esf. | Riesgos o notas |
|---|---|---|---|---|---|
| QA-1 | ✅ | Nativo | ztest en `native_sim`, twister | S | 32 casos verdes. |
| QA-2 | ✅ | Nativo | `west build` para `esp32s3_poe_eth_8di_8do/esp32s3/procpu` | S | 0 warnings en todo el build. FLASH 422 KB, DRAM 217 KB (54,5 %). |
| QA-3 | ✅ | Nativo | `native_sim` + sockets offloaded (`drivers/net/nsos_*.c`, `CONFIG_NET_NATIVE_OFFLOADED_SOCKETS`) + UART `zephyr,native-tty-uart` sobre un par pty de `socat` + GPIO emulados (`zephyr,gpio-emul`) | S | Hecho: 30 de 30 comprobaciones contra mosquitto y pymodbus. No ejercita el W5500 ni la UART real. |
| QA-4 | — | Nativo | Imagen de CI de Zephyr o `west twister` en GitHub Actions | S | — |

## Resumen: mayores huecos y riesgos de Zephyr para este producto

1. **Seguridad de plataforma (FW-2, FW-3, CFG-7):** el secure boot del ROM y el cifrado de flash del
   ESP32-S3 no están integrados en el flujo normal de Zephyr. Dependen del port Espressif de MCUboot,
   configurado aparte, y el cifrado impone bloques de escritura de 32 B a LittleFS y ZMS. Es el
   riesgo más serio para un producto industrial "de buen nivel".
2. **Dependencia de blobs y de Espressif para Wi-Fi (NET-4, NET-5):** la parte inalámbrica del
   ESP32-S3 en Zephyr depende de blobs binarios externos y del soporte de Espressif. Aquí no se pudo
   ni compilar (403 del proxy). Hay que validarlo pronto en un entorno con acceso.
3. **Integración UART/Modbus a medida:** el modo RS485 por hardware no encaja con el subsistema
   Modbus (que pisa el `flow_ctrl`), el subsistema genera un WRN por cada timeout y su temporizador
   RTU es fijo. Funciona con DE por GPIO, pero cualquier ajuste fino implica parchear upstream o
   mantener un fork.
4. **Menos "baterías incluidas" que ESP-IDF en la capa de aplicación:** no hay reconexión MQTT,
   Sparkplug, interfaz web con autenticación ni gestor de OTA HTTPS listo. Todo se construye con
   primitivas sólidas (sockets, nanopb, settings, LittleFS, servidor HTTP, MCUboot), con más código
   propio. A cambio, la base (devicetree, Kconfig, ztest/twister, `native_sim` para pruebas de punta a
   punta) es muy buena para calidad y mantenimiento.
5. **Hardware sin validar:** la polaridad de las DI y DO, el orden de color del LED y el
   comportamiento del W5500 a 20 MHz salen de la demo, del wiki y de la configuración de ESPHome, no
   de un esquemático (Waveshare no lo publica para esta placa). Ninguna línea se ejecutó en la placa
   real.
