# Gateway RS485 (Modbus RTU) → MQTT Sparkplug B — implementación ESP-IDF

Firmware del MVP (`docs/SPEC.md` §5) para la Waveshare **ESP32-S3-POE-ETH-8DI-8DO**.

- **Stack:** ESP-IDF **v6.1**, target `esp32s3`, esquema de configuración común §6.
- **Estudio de viabilidad de todo §4:** `docs/feasibility-idf.md`.

> **Advertencia: no se probó en hardware.** No había placa disponible. Lo que corrió de verdad es:
>
> - los tests de host;
> - el firmware real en QEMU (ESP32-S3), contra mosquitto y un esclavo pymodbus.
>
> El W5500, el modo RS485 por hardware de la UART, el TCA9554, las DI por GPIO, el LED y la PSRAM octal **solo están compilados**. Ver §6.

## 1. Arquitectura

### 1.1 Componentes (`components/`)

| Componente | Responsabilidad | Depende de IDF |
|---|---|---|
| `gw_core` | **Lógica pura en C, sin IDF; se prueba en el host:**<br>• `gw_config`: parseo y validación JSON §6.<br>• `gw_dout`: secuencia de arranque del TCA9554 y polaridad de las E/S.<br>• `mb_rtu`: CRC y tramas FC01–06/15/16.<br>• `mb_master`: transacciones, t3,5, timeout y reintentos.<br>• `gw_conv`: tipos (hasta 64 bits, `string` y bits de registro), escala, orden de bytes y datatype Sparkplug.<br>• `gw_mbap`: tramas Modbus TCP ↔ RTU de la pasarela (MB-15).<br>• `gw_plan`: lecturas en bloque.<br>• `gw_poll`: encuesta, fuera de línea y escrituras.<br>• `gw_rbe`: reporte por excepción y deadband.<br>• `gw_debounce`, `gw_backoff`, `gw_devstate`.<br>• `gw_localio`: política de las DO sin red: estado seguro al reiniciar y sin MQTT (IO-3), reglas locales DI→DO (IO-7); cuándo guardar los contadores de pulsos (IO-4).<br>• `gw_netsel`: interfaz activa entre Ethernet y Wi-Fi (NET-4).<br>• `gw_syslog`: líneas de log a RFC 5424 y anillo acotado (REL-4).<br>• `gw_tagdb`: imagen de proceso compartida.<br>• `gw_cfgdoc`: documento de configuración con los secretos aparte (exportar enmascarado, importar, migrar) y lector de JSON pegado en la consola (CFG-4, CFG-6, CFG-7).<br>• `gw_auth`: SHA-256, HMAC, PBKDF2 y Basic auth para la web (CFG-3).<br>• `gw_button`: gestos del botón BOOT (CFG-5, NET-5).<br>• `gw_buzz`: patrones del buzzer y alarmas con su demora y prioridad (IO-6).<br>• `spb`: codec Sparkplug con nanopb.<br>• `gw_node`: motor del Edge Node con births, deaths, seq, alias, NCMD/DCMD y rebirth. | No (solo `port/gw_port_esp.c`) |
| `nanopb` | Runtime nanopb 0.4.9.1 vendorizado (Zlib) | No |
| `gw_rs485` | UART1 en `UART_MODE_RS485_HALF_DUPLEX` (RTS = GPIO21), expuesta como `mb_transport_t` | Sí |
| `gw_bus` | Tarea **dueña única del bus RS485**: encuesta y escrituras DCMD | Sí |
| `gw_edge` | Tarea Sparkplug: cliente esp-mqtt, bdSeq, will, backoff, motor `gw_node` | Sí |
| `gw_net` | W5500 por SPI (u openeth en QEMU), Wi-Fi STA opcional como alternativa o respaldo (NET-4), DHCP o IP estática por interfaz, IPv6 opcional con SLAAC y dirección estática (NET-7), ruta por defecto según `gw_netsel`, SNTP | Sí |
| `gw_bsp` | **Descripción declarativa de la placa** (`gw_bsp.c`): todos los pines, direcciones I²C y niveles activos (DI, DO, DE del RS485, I²C, LED y orden de color, buzzer, W5500). Los drivers la leen y no contienen datos de la placa | Sí (tipos) |
| `gw_board` | 8 DI con muestreo a 1 kHz, debounce y contadores de pulsos (IO-4, en NVS); 8 DO en el TCA9554 (sobre `gw_dout`, sin pulsos al arrancar) con la tarea `local_io`, **única que escribe el expansor** (NCMD, reglas locales IO-7, estado seguro IO-3, reintentos); LED WS2812 de estado; buzzer en GPIO46 (IO-6, LEDC) | Sí |
| `gw_storage` | bdSeq en NVS; configuración desde LittleFS o embebida (solo si falta el archivo); escritura atómica de una configuración nueva; secretos en NVS cifrada (CFG-7); red de la última configuración válida para la recuperación; reset de fábrica y marca de puesta en marcha (CFG-5) | Sí |
| `gw_web` | Servidor HTTP: interfaz web embebida (CFG-3), API REST JSON de configuración y estado (CFG-4), Basic auth; mDNS y `_http._tcp` (NET-6) | Sí |
| `gw_ap` | Punto de acceso Wi-Fi de puesta en marcha (NET-5), aparte de `gw_net` | Sí |
| `gw_log` | Syslog remoto (REL-4): engancha `esp_log_set_vprintf`, copia cada línea a un anillo acotado y una tarea la envía por UDP | Sí |
| `gw_mbtcp` | Gateway Modbus TCP (MB-15): servidor TCP (lwIP) que pasa cada petición MBAP a la tarea dueña del bus y devuelve la respuesta del esclavo RTU. Solo con `modbus_tcp.enabled` | Sí |
| `gw_shell` | Shell `esp_console` en la consola: `status`, `devices`, `config [show\|load\|check]`, `restart`, `rebirth`, `mbread`, `ping` (y `di`, `boot`, `panic` en QEMU). Respuestas atómicas respecto del log | Sí |
| `main` | `app_main`: carga la configuración y conecta los componentes; modos normal, recuperación (configuración rechazada) y puesta en marcha (tras un reset de fábrica) | Sí |

Dependencias gestionadas, **con versión fija** (`==`) en cada `idf_component.yml` y hash en `dependencies.lock`:

- `espressif/mqtt` 1.1.0
- `espressif/w5500` 2.0.1
- `espressif/cjson` 1.7.19~2
- `espressif/led_strip` 3.1.1
- `joltwallet/littlefs` 1.23.1
- `espressif/mdns` 1.14.1

### 1.2 Tareas, colas y propiedad

```
                 esp_timer (1 kHz)                 consola UART0 (logs + shell)
                 gw_io: debounce DI ──┐
                                      │ flag + despertar
  ┌──────────────┐  cola escrituras   ▼            ┌──────────────────────────┐
  │ rs485_bus    │◄── (8 × 16 B) ──── sparkplug ───►│ esp-mqtt (tarea propia)  │
  │ núcleo 1     │                    núcleo 0      │ lwIP / esp_eth (núcleo 0)│
  │ prio 10      │── dirty bit ─────► prio 8        └──────────────────────────┘
  │ mb_master    │   por dispositivo  gw_node        ▲ MQTT_EVENT_DATA → pool de
  │ gw_poller    │── fin escritura ─► (cola 24×4 B)  │ 4 búferes de comando (1 KB)
  └──────┬───────┘                                   │
         │ escribe                    lee            │
         ▼                                           │
      gw_tagdb (imagen de proceso, mutex corto) ◄────┘   status_led (prio 2)
```

| Tarea | Núcleo / prio | Pila | Vigilada por TWDT | Qué posee |
|---|---|---|---|---|
| `rs485_bus` (`gw_bus`) | 1 / 10 | 4 KB estática | Sí; se alimenta en cada intento Modbus | `mb_master_t`, `gw_poller_t`, la UART1. **Única** tarea que toca el bus (MB-16) |
| `sparkplug` (`gw_edge`) | 0 / 8 | 6 KB estática | Sí; se alimenta tras **cada publicación** y despierta como mínimo cada 1 s | Cliente MQTT, `gw_node_t` (seq, alias, estado RBE). Los NCMD sobre las DO se piden a `local_io` (`gw_io_set_do`, espera hasta 500 ms) |
| `local_io` (`gw_io`) | — / 9 | 4 KB estática | Sí; despierta como mínimo cada 100 ms | El TCA9554 desde que arranca con la configuración válida: escrituras de los NCMD (cola de 2), reglas locales DI→DO (IO-7; el muestreo de las DI la despierta con una notificación), estado seguro tras `failsafe_s` sin sesión (IO-3, lee `GW_ST_MQTT_UP`) y reintentos del expansor que no responde. Guarda los contadores de pulsos (IO-4) cada `counter_save_s`. Antes, `app_main` deja las DO en su estado de arranque |
| `esp_timer` (IDF) | — | IDF | Sí, como usuario del TWDT (`di_sample`): el callback de muestreo lo alimenta cada 1 s | Muestreo de DI (callback de 1 ms) |
| `status_led` | — / 2 | 2,5 KB | No (no crítica) | El RMT del LED |
| `console` (`gw_shell`) | — / 2 | 4 KB | No (no crítica) | La UART de la consola (REPL `esp_console`); lee `gw_tagdb` y `gw_status`; las escrituras y la lectura Modbus pasan por `gw_storage`, `gw_edge` y la cola del bus |
| `syslog` (`gw_log`, solo con `syslog.host`) | — / 2 | 3,5 KB estática | No (no crítica) | El socket UDP y el lado consumidor del anillo |
| `modbus_tcp` (`gw_mbtcp`, solo con `modbus_tcp.enabled`) | 0 / 3 | 4 KB estática | No (no crítica; toda espera acotada: `select` de 1 s, envío de 2 s, respuesta del bus de 8 s) | El socket de escucha, hasta 2 clientes (`CONFIG_GW_MBTCP_MAX_CLIENTS`) y la única transacción en curso, que ejecuta `rs485_bus` |
| `httpd` (`gw_web`) | — / 3 | 8 KB | No (no crítica) | El servidor HTTP; valida y guarda configuraciones (búferes en PSRAM si hay) |
| `boot_button` (`gw_board`) | — / 2 | 3 KB estática | No (no crítica) | GPIO0 (muestreo cada 20 ms); pide los pitidos de confirmación al buzzer |
| `buzzer` (`gw_board`) | — / 2 | 2,5 KB estática | No (no crítica) | GPIO46 y el canal 7 de LEDC (IO-6): cada 20 ms lee las banderas de `gw_status`, decide la alarma (`gw_buzz`) y prende o apaga el tono |
| `mdns` (componente) | 0 / 1 | 4 KB | No | El responder mDNS |
| esp-mqtt (`mqtt_task`), lwIP (`tcpip`), RX de Ethernet (`w5500_tsk` / `emac_rx`), Wi-Fi (solo con `network.wifi`), event loop (`sys_evt`) | **0** / IDF | IDF | IDLE vigiladas | El event loop es dueño de `gw_netsel` (eventos de enlace e IP y el temporizador de *failback*) |

**Afinidad.** Toda la pila de red corre en el núcleo 0, lejos del bus RS485 del núcleo 1: `CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0`, `CONFIG_MQTT_USE_CORE_0`, `ETH_MAC_FLAG_PIN_TO_CORE` en `gw_net` (el driver se instala desde `app_main`, en el núcleo 0) y el event loop por defecto y la tarea del Wi-Fi, que IDF ya fija al 0. El comando `status` del shell muestra el núcleo de cada tarea (`tasks rs485_bus 1 sparkplug 0 tcpip 0 …`); el E2E lo comprueba.

**Concurrencia.** Los productores nunca bloquean al consumidor:

- Las actualizaciones de dispositivos y de DI son **flags atómicos** más un evento de "despertar" opcional. Perder el despertar no pierde estado: la tarea Sparkplug reconcilia en cada vuelta.
- Lo que no puede perderse va por colas estáticas acotadas:
  - comandos (pool de 4 búferes);
  - confirmaciones de escritura;
  - escrituras DCMD hacia el bus (cola de 8).
- La única memoria compartida es `gw_tagdb`. Se protege con un mutex y solo se copia dentro de la sección crítica.

**Conexión MQTT y Sparkplug:**

- La auto-reconexión de esp-mqtt está desactivada.
- Antes de **cada** intento se toma un bdSeq nuevo de NVS, se arma el NDEATH correspondiente como *will* (`esp_mqtt_set_config`) y se reconecta tras un backoff exponencial con jitter (1–60 s).
- El backoff vuelve al mínimo **solo cuando la sesión se mantuvo estable** un keepalive completo (como mucho 60 s) después del NBIRTH, no con cada CONNACK: un broker que acepta la conexión y la corta enseguida (ACL, client_id duplicado, límite de conexiones) ve crecer los intentos hasta el tope. Un fallo de `esp_mqtt_client_init` (falta de memoria) espera el mismo backoff.
- Al conectar se envía un SUBSCRIBE (NCMD y DCMD/+) y **se espera su SUBACK** (hasta 5 s) antes del NBIRTH y los DBIRTH: un nodo anunciado en línea siempre recibe sus comandos. Si el SUBSCRIBE no se puede enviar, el broker rechaza un tema (código 0x80) o el SUBACK no llega, se cierra la conexión con DISCONNECT (el broker descarta el *will*: no hubo NBIRTH) y se reintenta con backoff.
- Si el reloj se sincroniza después del NBIRTH, se hace un *rebirth* para que los timestamps sean válidos.
- **Cambio de interfaz (NET-4).** Con Ethernet y Wi-Fi configurados, el socket MQTT se ata a la interfaz activa (`SO_BINDTODEVICE`). Cuando `gw_netsel` elige otra, la sesión no puede migrar: se publica el NDEATH de forma ordenada (si la interfaz vieja ya no anda, se pierde y el broker publica el *will* después), se destruye el cliente y se conecta uno nuevo por la interfaz nueva, con bdSeq nuevo, NBIRTH y todos los DBIRTH. Con solo Ethernet no hay atadura ni cambios: todo sigue como en el MVP.

**Selección de interfaz (`gw_netsel`, NET-4):**

- Una interfaz es *utilizable* con enlace (cable o asociación) y una dirección IP.
- Cada interfaz tiene un rango: con `role: backup`, Ethernet es la preferida; con `primary`, Wi-Fi.
- Sin interfaz activa, la primera utilizable pasa a activa enseguida, sea cual sea su rango.
- Si la activa deja de ser utilizable, la mejor utilizable la reemplaza enseguida (*failover*).
- La preferida recupera la sesión solo después de `failback_s` segundos seguidos utilizable (*failback* con histéresis: un cable que va y viene no corta la sesión en cada rebote).
- La ruta por defecto (y con ella DNS y SNTP, `CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF`) sigue a la interfaz activa. Las prioridades de ruta de `esp_netif` se fijan en el mismo orden (Ethernet 50; Wi-Fi 20 como respaldo o 100 como primaria): por defecto STA tiene 100 y ganaría.

### 1.3 Memoria (REL-2)

- **Límites estáticos:** 16 dispositivos, 64 tags por dispositivo, 256 tags en total.
- **Toda la memoria propia es de tamaño fijo**, estática o asignada una sola vez al arrancar:
  - **en PSRAM si la hay** (`gw_storage_big_alloc`, interna si no: QEMU o PSRAM no detectada): la configuración `gw_config_t` (unos 26 KB) y el motor `gw_node_t` (unos 27 KB, con su búfer de payload de 6 KB);
  - `.bss` interna: `gw_edge` con la pila de 6 KB y 4 búferes de comando de 1,2 KB; `gw_bus` unos 15 KB (planificador, encuesta y pila de 4 KB); `gw_core` unos 15 KB (`gw_tagdb`, 56 B por tag desde MB-6); `gw_mbtcp` unos 6 KB (pila de 4 KB y búferes, aunque esté apagado).
- **Por qué no `EXT_RAM_BSS_ATTR`:** poner `.bss` en PSRAM exige `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`, que en IDF es incompatible con `CONFIG_SPIRAM_IGNORE_NOTFOUND`: una placa cuya PSRAM no se detecte no arrancaría. Con la asignación al arrancar el equipo sigue funcionando con RAM interna (y la build de QEMU, sin PSRAM, es la misma).
- Sin `malloc` en encuesta, codificación ni publicación.
- Se asigna memoria solo al arrancar: esas dos estructuras, la lectura de LittleFS y el árbol de cJSON, todo en PSRAM si la hay (§3.4).
- lwIP y esp-mqtt asignan internamente (ver §6).

## 2. Datos de la placa marcados "verificar"

**Fuentes:**

- El demo oficial `ESP32-S3-POE-ETH-8DI-8DO-Demo.zip` (Arduino, `files.waveshare.com/wiki/ESP32-S3-POE-ETH-8DI-8DO/`).
- La wiki de Waveshare.
- La definición ESPHome de esta misma placa, `Waveshare-ESP32-S3-ETH-8DI-8DO` (`esphome-devices`, `config.yaml`).
- La definición ESPHome de la placa hermana `Waveshare-ESP32-S3-ETH-8DI-8RO` (mismos GPIO de DI, I²C, RTC, LED y buzzer; sus relés **no** están invertidos, así que no sirve como evidencia de la polaridad de las DO).

No se encontró esquemático publicado.

| Dato | Hallazgo | Evidencia | Confianza |
|---|---|---|---|
| Expansor de las DO | **TCA9554 en 0x20** | Demo: `TCA9554_ADDRESS 0x20` (`WS_TCA9554PWR.h`). ESPHome de la hermana: `pca9554 address: 0x20` | Alta |
| Polaridad de las DO | **Nivel bajo en el pin del expansor = DO activa** (salida Darlington) | **A favor:** el demo arranca con `Dout_Init()` → `TCA9554PWR_Init(0x00, 0xFF)`, todas en alto (un demo no energiza 8 cargas al encender), y su modo DI→DO (`WS_DIN.cpp`, `DIN_Inverse_Enable = 0`) copia el nivel crudo (DI en reposo = 1 → DO = 1 = apagada). El ESPHome de esta placa (8DI-8DO) declara `inverted: true` en las 8 DO. La implementación Zephyr llegó a lo mismo (`GPIO_ACTIVE_LOW`). **En contra:** el demo llama `Dout_Open()` al nivel ALTO, `"CH1":1` por MQTT pone el pin en alto y la wiki (copiada de la de relés) dice "ALL_ON … high level (on state)" | **Media.** Configurable: `CONFIG_GW_DO_ACTIVE_LOW` (por defecto `y`, la hipótesis más probable). **Medir con un multímetro antes de conectar cargas** |
| RTC | **PCF85063 en 0x51** (SDA 42, SCL 41, INT 40) | Demo: `PCF85063_ADDRESS (0x51)` (`WS_PCF85063.h`); ESPHome: `platform: pcf85063` | Alta (no se usa en el MVP) |
| Polaridad de las DI | GPIO con pull-up; **entrada activa = nivel 0** (se invierte) | Demo: `pinMode(DIN_PIN_CHx, INPUT_PULLUP)` y estado de reposo `0xFF`; ESPHome: `INPUT_PULLUP` + `inverted: true` | Media-alta. Configurable: `CONFIG_GW_DI_ACTIVE_LOW` |
| Buzzer | **Pasivo** (necesita PWM) | Demo: `ledcAttach(46, 1000 Hz, 8 bits)` con duty 200/255; ESPHome usa `rtttl` sobre `ledc` | Media. Se usa con LEDC a 2,7 kHz (`CONFIG_GW_BUZZER_FREQ_HZ`); un buzzer activo se manejaría por nivel (`gw_bsp.buzzer.passive = false`) |
| LED | WS2812 en GPIO38, **orden de bytes RGB** (no GRB) | Demo: `neopixelWrite(pin, green, red, blue)` (intercambia R y G); ESPHome: `rgb_order: RGB` | Media |
| RS485 | UART1, TX 17, RX 18, RTS 21, modo `UART_MODE_RS485_HALF_DUPLEX` | Demo `WS_RS485.cpp`: `setPins(-1,-1,-1,21)` + `setMode(UART_MODE_RS485_HALF_DUPLEX)` | Alta |
| W5500 | SPI 13/14/15, CS 16, INT 12, RST 39 | Wiki y `WS_ETH.h` | Alta |
| Consola | USB-C → puente USB-UART CH34x en UART0 (GPIO 43/44) | La wiki enlaza el driver CH34XSER | Media. Si no hay logs, probar con `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` (el shell también funciona ahí) |

Todos estos datos están en un solo lugar, `components/gw_bsp/gw_bsp.c`. Las dos polaridades que solo se deducen de forma indirecta (DI y DO) se pueden invertir en `menuconfig` (menú `Gateway: board`); el resto es fijo. Arranque de las DO (`gw_dout`): el latch se carga con el nivel OFF de cada canal (0xFF con activa en bajo) **antes** de pasar los pines a salida; la escritura y la relectura se invierten según la máscara de polaridad.

**Pines de *strapping*** con periféricos de la placa: GPIO0 (BOOT), GPIO3 (RX del CAN, JTAG), GPIO45 (SD D0) y GPIO46 (buzzer). El firmware configura GPIO0 y GPIO46 recién desde `app_main`, nunca antes; GPIO3 y GPIO45 no se usan.

## 3. Compilar, grabar y configurar

### 3.1 ESP-IDF

```bash
mkdir -p ~/sdk/idf && cd ~/sdk/idf
git clone --depth 1 --branch v6.1 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git
cd esp-idf && IDF_TOOLS_PATH=~/sdk/idf/tools ./install.sh esp32s3
export IDF_TOOLS_PATH=~/sdk/idf/tools && . ~/sdk/idf/esp-idf/export.sh
```

### 3.2 Firmware

```bash
cd idf
idf.py set-target esp32s3 build      # QA-2: 0 warnings
idf.py size
idf.py -p /dev/ttyUSB0 flash monitor # graba bootloader, tabla, app (ota_0) y la partición LittleFS
```

Resultado de `idf.py size` (build limpia con `idf.py set-target esp32s3 build`, 1198/1198 objetivos, 0 warnings, `sdkconfig` regenerado desde `sdkconfig.defaults`; Wi-Fi compilado, `CONFIG_GW_WIFI=y`):

| Región | Usado |
|---|---|
| Flash code (`.text`) | 887 980 B |
| Flash data (`.rodata`, etc.) | 227 352 B |
| DIRAM | 180 164 B de 341 760 (**52,7 %**): `.bss` 90 128, `.text` 65 707, `.data` 24 329 |
| IRAM | 16 384 B (100 %, lo normal en S3: el resto del código IRAM va en DIRAM) |
| Imagen total | **1 221 788 B** (29 % de la partición `ota_0` de 4 MB) |

**Costo del trabajo Modbus (MB-6, MB-11, MB-12, MB-14, MB-15, SPB-15):** medido sobre `main` con config-web, `.bss` +22 544 B (67 584 → 90 128) e imagen +11 596 B. Medido antes sobre el MVP: `.bss` +19 824 B (57 792 → 77 616). Unos 13 KB son el valor de tag de 56 B (antes 24) en la imagen de proceso (`gw_core` +8,9 KB), la copia de trabajo de la encuesta y la cola de escrituras (`gw_bus` +4,6 KB, también el planificador por clases) y la copia del shell (+2 KB); 5,8 KB son el servidor Modbus TCP (`gw_mbtcp`), reservado aunque esté apagado. Flash +13 KB.

La configuración y el motor Sparkplug (unos 53 KB) se asignan al arrancar, en PSRAM si la hay (§1.3), así que no figuran en `.bss`.

**Costo de la configuración por web, shell, puesta en marcha y secretos (CFG-3 a CFG-7, NET-5, NET-6):** +113 116 B de imagen y +10 408 B de DIRAM (`.bss` +9 792: pila estática de 3 KB del botón, búfer de 1 KB de las respuestas del shell, búfer de estado de 1 KB y valores de tags de la web, mutex y semáforos). En ejecución, el servidor HTTP (pila de 8 KB y sockets), mDNS (pila de 4 KB) y los búferes de una configuración en validación (el documento, `gw_config_t` de 26 KB y el árbol cJSON) se asignan del heap, este último solo mientras se guarda y en PSRAM si hay.

**Costo del Wi-Fi (NET-4).** Antes: DIRAM 153 121 B (44,8 %) e imagen de 735 029 B. El driver Wi-Fi suma **27 327 B de DIRAM estática** (medido en su commit) (`.bss` +11 504, `.text` +8 352, `.data` +7 471) aunque la configuración no lo active, y 347 KB de flash. Ya se sacó de IRAM lo que IDF permite (`CONFIG_ESP_WIFI_IRAM_OPT=n`, `CONFIG_ESP_WIFI_RX_IRAM_OPT=n`). Al arrancarlo, sus búferes dinámicos y los de lwIP van a PSRAM cuando se puede (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`); el heap interno que consume en ejecución no se midió (no hay placa). Quien no lo necesite recupera todo con `CONFIG_GW_WIFI=n`.

**Costo del syslog remoto (REL-4):** 4,6 KB de `.bss` (pila estática de 3,5 KB de la tarea emisora y los búferes de una línea y un datagrama). El anillo (32 líneas de unos 240 B) se asigna al arrancar y solo si `syslog.host` está puesto, en PSRAM si la hay.

Tabla de particiones (`partitions.csv`, 16 MB): `nvs` 24 KB, `otadata`, `phy_init`, `ota_0`/`ota_1` 4 MB cada una (reservadas para FW-1), `storage` LittleFS 1 MB y `coredump` 64 KB.

**Core dump.** Un *panic* (o el TWDT) guarda un volcado ELF en la partición `coredump` (desde IDF 6.0 ELF es el único formato; la opción `CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF` ya no existe). Se lee con `idf.py coredump-info` (o `coredump-debug`) con la placa conectada. Verificado en QEMU: el comando `panic` del shell (solo en la build de emulador) provoca un `abort()`, el arranque siguiente informa `panic` y `esp_coredump info_corefile` decodifica el volcado de la partición con la traza a través de `cmd_panic` (con la salvedad de §4.3 sobre el otro núcleo en QEMU). En la placa no se probó.

### 3.3 Actualizar un `sdkconfig` existente (nota de migración)

`sdkconfig.defaults` solo se aplica al **crear** el `sdkconfig`. Con un `sdkconfig` de una versión anterior:

- **Antes de la descripción declarativa de la placa (`gw_bsp`):** `reconfigure` descarta **sin aviso** 17 opciones de pines (6 de `gw_board`, 7 de `gw_net` y 4 de `gw_rs485`; los pines ahora son fijos en `gw_bsp.c`) y aparece `CONFIG_GW_DO_ACTIVE_LOW=y`, que **invierte la polaridad de las DO** respecto de una build ya probada.
- **Opciones agregadas después** no llegan a un `sdkconfig` existente: `CONFIG_UART_ISR_IN_IRAM`, la afinidad de lwIP y esp-mqtt al núcleo 0 (`CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0`, `CONFIG_MQTT_USE_CORE_0`).

Tras actualizar, lo más simple es **borrar `idf/sdkconfig`** (o `idf.py set-target esp32s3`, que lo regenera). Si se conserva, revisar en `menuconfig` al menos `CONFIG_GW_DO_ACTIVE_LOW` y las opciones de arriba. No hay equipos desplegados, así que no se agregó una migración automática.

### 3.4 Configuración

1. El firmware embebe `components/gw_storage/default_config.json`, que es el ejemplo de §6.
2. Para reemplazarla, copie el JSON a `idf/fs/config.json` y edítelo. `idf.py flash` graba la carpeta `fs/` en la partición `storage` y el firmware lee `/cfg/config.json` al arrancar.
3. Las claves opcionales fuera del MVP (`network.wifi`, NET-4; `network.ipv6`, NET-7; `local_io.failsafe_s` y `do_failsafe`, IO-3; `local_io.counter_save_s`, IO-4; `local_io.buzzer`, IO-6; `local_io.rules`, IO-7; `syslog`, REL-4; `modbus_tcp`, MB-15; tipos de 64 bits, `string`, órdenes de bytes y `bit` en los tags, MB-6) están en `docs/SPEC.md` §6.1.
4. **La configuración embebida se usa solo si `/cfg/config.json` no existe** (y no hubo un reset de fábrica, §3.4). Cualquier otro problema deja el equipo en **recuperación**: la partición LittleFS no monta, el archivo está vacío, no se puede leer, supera el límite o es inválido. Entonces **no arranca la encuesta ni MQTT**; sí arrancan la red (con la de la última configuración válida, o DHCP por Ethernet), la interfaz web, el shell y el punto de acceso (NET-5), para corregirla sin regrabar. El LED parpadea en rojo y el log repite cada 10 s, con nivel ERROR, el motivo, por ejemplo:
   - `/cfg/config.json: 140000 bytes; el máximo es 131072 (CONFIG_GW_CONFIG_MAX_BYTES)`;
   - `partición LittleFS 'storage' no montable (ESP_FAIL)`;
   - `devices[0].tags[1].writable: solo las áreas coil y holding son escribibles`.

   Los errores semánticos llevan la **ruta JSON** del valor (`devices[1].tags[2].type: valor no válido (se esperaba bool|uint16|…)`, `devices[0].tags[0].scal: clave desconocida`). Los de sintaxis llevan **línea y columna**, contadas en caracteres a partir del puntero de error de cJSON (`JSON mal formado en la línea 5, columna 3: sintaxis no válida`); también se rechaza el contenido que sobra después del documento. Las **claves duplicadas** en cualquier objeto se rechazan (`mqtt.host: clave duplicada`): cJSON las acepta y sus búsquedas solo ven la primera.
5. **Límite de tamaño: 128 KiB** (`CONFIG_GW_CONFIG_MAX_BYTES`, menú `Gateway: storage`). El máximo del esquema (16 dispositivos, 256 tags con todos los campos) ocupa unos 46 KB en JSON compacto y 82 KB con sangría de 2 espacios. Al arrancar se asignan el búfer del archivo y el árbol de cJSON (unos 200 KB para 256 tags), **en PSRAM** si la hay; se liberan antes de arrancar las tareas. El log informa el heap libre antes de analizar (`/cfg/config.json: N bytes; free heap: internal … (largest block …), PSRAM …`).
6. **Sin PSRAM** (QEMU, o una placa cuya PSRAM no se detecte: `SPIRAM_IGNORE_NOTFOUND`) todo sale del heap interno. Medido en QEMU, con unos 315 KB de heap interno libre al cargar:

   | Configuración (16 dispositivos, tags con todos los campos) | Archivo | Resultado |
   |---|---|---|
   | 256 tags (máximo del esquema), JSON compacto | 46 052 B | Carga |
   | 256 tags, sangría de 2 espacios | 82 450 B | Carga |
   | 256 tags, con espacios de relleno | hasta 115 877 B | Carga |
   | 256 tags, con espacios de relleno | desde 117 396 B | Se rechaza: `memoria insuficiente para analizar … bytes de JSON` |

   Es decir: sin PSRAM entra cualquier configuración legal, **siempre que el archivo no pase de unos 110 KB** (menos tags dejan algo más de margen). Con PSRAM el límite es solo `CONFIG_GW_CONFIG_MAX_BYTES`. En la placa sin PSRAM el heap libre puede diferir del de QEMU; el log de arranque lo informa.

6. **Cambiar la configuración sin regrabar:** la interfaz web (`http://<ip>/` o `http://<hostname>.local/`), la API REST (`PUT /api/config`, SPEC §6.1) o el shell (`config load`, pegar el JSON). En los tres casos se valida igual, se guarda de forma atómica (archivo temporal, `fsync`, `rename`) y el equipo se reinicia de forma ordenada (NDEATH). `config show` y `GET /api/config` exportan con los secretos enmascarados.

Opciones de compilación en `idf.py menuconfig`, en los menús `Gateway: ...`:

- polaridad de las DI y de las DO (los pines son fijos, en `gw_bsp`);
- prioridades y núcleos de las tareas;
- lecturas en bloque y hueco máximo (valores por defecto: cada dispositivo puede fijar los suyos con `block_reads` y `max_gap`);
- FC15/FC16 forzadas;
- intervalo máximo entre sondeos de un dispositivo fuera de línea (`GW_MB_OFFLINE_PROBE_MAX_MS`);
- backoff MQTT;
- Wi-Fi compilado o no (`CONFIG_GW_WIFI`, menú `Gateway: network`; sin él, `network.wifi` se ignora con un warning);
- espera de SNTP;
- líneas que guarda el syslog remoto (`CONFIG_GW_SYSLOG_SLOTS`, 32 por defecto) y prioridad de su tarea;
- tamaños de cola;
- gateway Modbus TCP: clientes simultáneos, prioridad y espera máxima de una respuesta del bus (menú `Gateway: Modbus TCP gateway`);
- comandos del shell que cambian algo (`CONFIG_GW_SHELL_WRITE_COMMANDS`), punto de acceso (`CONFIG_GW_SOFTAP`, minutos tras BOOT) y mDNS (`CONFIG_GW_MDNS`).

**LED de estado** (de mayor a menor prioridad):

| Estado | Patrón |
|---|---|
| Reset de fábrica en curso | Blanco fijo |
| BOOT presionado más de 2 s | Blanco, parpadeo de 4 Hz |
| Puesta en marcha (sin configuración tras un reset) | Azul, parpadeo de 1 Hz |
| Configuración inválida (recuperación) | Rojo, parpadeo de 4 Hz |
| Sin red (sin IP) | Rojo fijo |
| Sin broker | Ámbar, parpadeo de 1 Hz |
| Dispositivo Modbus fuera de línea | Verde y naranja alternados (1 Hz) |
| Operando | Verde fijo |

### 3.4 Puesta en marcha, reset de fábrica y secretos

**Acceso.** La web y la API piden HTTP Basic con `web.username` y `web.password`. Mientras no haya una contraseña web, se entra con el usuario `admin` y la **contraseña de puesta en marcha**: 12 caracteres aleatorios por equipo, creados en el primer arranque y guardados en NVS. Se ve en la consola en cada arranque (`web: no web password set; user 'admin', commissioning password '…'`; con `printf`, nunca por syslog). En recuperación y en puesta en marcha vale además la contraseña web guardada, con cualquier usuario (el usuario está en el documento rechazado).

**Procedimiento en sitio:**

1. Conectar Ethernet (DHCP) o, si no hay red, el punto de acceso: con la configuración rechazada o en puesta en marcha arranca solo; con una configuración válida, BOOT presionado entre 2 y 10 s lo levanta por 30 min (`CONFIG_GW_SOFTAP_TIMEOUT_MIN`). Red `GW-Setup-XXXXXX` (últimos bytes de la MAC), WPA2 con la contraseña de puesta en marcha; la web queda en `http://192.168.4.1/`.
2. Abrir `http://<ip>/` (o `http://<hostname>.local/`, NET-6), cargar o editar la configuración (formularios, pestaña JSON o Importar), Validar y Guardar. El equipo se reinicia con ella.
3. Poner una `web.password` propia (al menos 8 caracteres).

**Reset de fábrica (CFG-5):** BOOT presionado 10 s con el equipo andando (GPIO0 es *strapping*: si está presionado al encender, el ROM entra en modo descarga; el firmware lo lee solo después y exige verlo suelto una vez). El LED pasa a blanco a los 2 s; a los 10 s suenan dos pitidos, se borran la partición LittleFS completa y NVS (configuración, secretos, bdSeq, contraseña de puesta en marcha) y el equipo se reinicia de forma ordenada en **modo puesta en marcha**: no usa la configuración embebida de ejemplo ni su identidad Sparkplug; espera una configuración por la web o el shell, con una contraseña de puesta en marcha nueva. En QEMU el botón se simula con `boot 1` / `boot 0`.

**Secretos (CFG-7):** `mqtt.password`, `network.wifi.password` y el hash de `web.password` (PBKDF2-HMAC-SHA256, 2048 iteraciones, sal aleatoria) están en NVS, nunca en `/cfg/config.json`, en las dos builds:

| Build | Cómo | NVS | eFuses |
|---|---|---|---|
| **Por defecto** (desarrollo) | `idf.py set-target esp32s3 build` | **Sin cifrar**: los secretos quedan fuera del JSON, pero legibles con un volcado de la flash. El shell avisa `secrets  NVS NOT encrypted` | No toca ninguno: se puede grabar en cualquier placa |
| **Producción** | `idf.py -B build-prod -D SDKCONFIG=build-prod/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.production" build` | Cifrada con el esquema HMAC (`CONFIG_NVS_ENCRYPTION`, `CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC`, clave en el bloque eFuse **KEY5**), sin exigir cifrado de flash. El shell dice `secrets  NVS encrypted (HMAC key in eFuse KEY5)` | **Quema KEY5 en el primer arranque** |

`sdkconfig.production` es el overlay de producción: ahí irán también secure boot (FW-2) y cifrado de flash (FW-3). La build de QEMU (`sdkconfig.qemu`) activa el mismo cifrado sobre el eFuse emulado de cada corrida, así que el E2E lo ejercita sin quemar nada real.

- **Proceso de fábrica (solo con la build de producción).** El primer arranque genera una clave HMAC aleatoria y la **quema en KEY5 con propósito `HMAC_UP`: es irreversible** y la clave no se puede leer después (las claves de NVS se derivan con el periférico HMAC). Ese bloque queda ocupado: secure boot (FW-2) y cifrado de flash (FW-3) deben usar otros. Si se prefiere quemarla en la línea de producción: `espefuse.py burn_key BLOCK_KEY5 clave.bin HMAC_UP` antes del primer arranque. Una placa con KEY5 ya usado para otra cosa no arranca NVS (el log lo dice y bdSeq queda en RAM).
- **Placas de desarrollo.** Grabar siempre la build por defecto: no quema eFuses. Grabar una vez la de producción deja KEY5 ocupado para siempre en esa placa. Pasar de la build por defecto a la de producción en un mismo equipo borra NVS (ver Migración).
- **Migración.** Un `config.json` con secretos en claro (grabado con `idf.py flash` o de un firmware anterior) se acepta: al arrancar, los secretos pasan a NVS, se **borra la partición LittleFS entera** (LittleFS es *copy-on-write* y los bloques viejos guardarían el texto) y se reescribe el archivo sin ellos. Durante esa ventana el equipo queda marcado para puesta en marcha: un corte de energía no lo deja con la identidad de ejemplo. Al activar el cifrado en un equipo que ya tenía NVS sin cifrar, NVS se borra (bdSeq y secretos): hay que volver a cargar los secretos (PUT con los valores).

## 4. Verificación (evidencia real)

### 4.1 QA-1: tests de host (CMake + Unity v2.6.1 + CTest)

```bash
cmake -S idf/test/host -B build-host && cmake --build build-host -j3 && ctest --test-dir build-host
```

Resultado: **16/16 ejecutables y 143/143 casos OK**, compilados con `-Wall -Wextra -Werror -Wconversion`.

| Suite | Casos | Qué cubre |
|---|---|---|
| `test_mb_rtu` | 6 | CRC (vectores de la especificación Modbus), tramas FC01–06/15/16, longitud de respuesta, excepciones, eco erróneo |
| `test_mb_master` | 9 | t3,5 y retardo entre tramas, timeout y reintentos, CRC malo, unidad equivocada, excepción sin reintento, espera del fin de transmisión proporcional a la trama (FC16 de 13 B a 1200 baudios con paridad y la FC16 más larga) |
| `test_conv` | 10 | Mapeo de datatypes §6 (también Int64/UInt64/Double/String y escalados de 64 bits a Double), int16/uint16/int32/uint32/float32, los cuatro órdenes ABCD/CDAB/BADC/DCBA en 32 y 64 bits, 2^53 + 1 exacto, INT64_MIN, float64, textos (relleno, NUL, no imprimibles, BADC, largo máximo), escala inversa (12,3 → 123), rangos |
| `test_logic` | 7 | RBE y deadband contra el último valor reportado (deriva), RBE exacto de 64 bits y de textos (hash), validez y NaN, debounce, backoff 1–60 s, fuera de línea |
| `test_config` | 22 | Sección `web` (usuario, contraseña, puerto), `network.hostname` y `mdns` (validación y nombre derivado), campos del error (ruta, línea, columna) y anidamiento máximo; ejemplo §6 literal, sección `syslog`, sección `network.wifi` (valores por defecto, SSID, contraseña WPA2/PSK/abierta, rol, `failback_s`, IP estática), literales IPv6 (`::`, grupos de 4 dígitos, IPv4 embebida, 16 formas inválidas) y sección `network.ipv6`, JSON embebido, valores por defecto, reglas del esquema, errores con ruta (`devices[1].tags[2].type`), sintaxis con línea y columna (también con UTF-8 y contenido sobrante), claves duplicadas, límites 16/64/256, documento máximo (256 tags, > 40 KB), asignador del árbol y falta de memoria, MB-6 (tipos, `bit`, `length`, órdenes), `block_reads`/`max_gap`, `scan`/`slow_poll_ms`, sección `modbus_tcp` |
| `test_plan` | 3 | Lecturas en bloque, solapes, huecos, límite de 125 registros; `block_reads`/`max_gap` por dispositivo contra los valores por defecto de la build (#38) |
| `test_spb` | 8 | Ida y vuelta de todos los tipos, enteros de 64 bits exactos, formato wire (Int16 con extensión de signo), NDEATH sin seq, peor caso de DBIRTH en el búfer |
| `test_node` | 15 | NBIRTH y DBIRTH (nombre, alias, datatype, valor; alias únicos), DDEATH, DDATA por excepción, seq compartido con vuelta a 0, rebirth, NCMD DO/Reboot, DCMD por nombre y alias, descartes (el log muestra el alias solo si venía), comando abortado a mitad del payload sin aplicar nada, DCMD rechazado entero si la cola del bus no tiene lugar (#60), DO `is_null` sin expansor y NDATA al recuperarlo, `Node Control/Scan Rate` (SPB-15), métricas y DCMD MB-6 exactos |
| `test_poll` | 23 | Encuesta contra un esclavo simulado, fuera de línea tras N ciclos, excepción, un bloque sin respuesta (ciclo fallido, tags sin refrescar, DDEATH, sin vuelta a línea con el ciclo incompleto), contadores por dispositivo, planificación, escritura FC06 con relectura, FC15/FC16 forzadas, NaN e infinito rechazados en coils, tag fuera de rango, sondeo con backoff de los fuera de línea (un intento, 2–4–8–10 s, vuelta a línea) y un equipo sano que mantiene su periodo junto a tres muertos; nunca respondió: sondeado desde el arranque (#61); MB-6 (decodificación en sus órdenes, escrituras exactas, bit por lectura-modificación-escritura); escaneo rápido y lento (MB-12); escritura entre peticiones de un ciclo con su latencia (MB-14); Scan Rate (SPB-15); reloj inyectado en el test del bloque sin respuesta (#66); transacciones Modbus TCP contadas por unidad y en la pasarela (#69) |
| `test_netsel` | 5 | Selección de interfaz (NET-4): solo Ethernet (sin conmutaciones), Wi-Fi de respaldo (*failover* inmediato, *failback* tras `failback_s`), Wi-Fi primaria, enlace preferido que rebota (no roba la sesión), `failback_s` = 0 |
| `test_syslog` | 5 | REL-4: severidad del formato de `esp_log`, parseo de líneas (colores, hora del sistema, `: ` en el mensaje, truncado), timestamps RFC 3339 (bisiesto, 2099), formato RFC 5424 (PRI, NILVALUE, saneado de HOSTNAME/APP-NAME, caracteres de control, truncado), anillo lleno sin esperar y conteo de descartes |
| `test_auth` | 5 | SHA-256 (FIPS 180-4, bordes de relleno, un millón de `a` en trozos), HMAC (RFC 4231), PBKDF2 (RFC 7914 y vectores publicados), hash de contraseña con sal (igual que `hashlib`), Basic auth |
| `test_cfgdoc` | 7 | Secretos: PUT nuevo (documento sin secretos), máscara que conserva, ausente o `""` que borra, archivo con secretos en claro (migración), errores con ruta, exportación enmascarada y reimportación, lector de JSON de la consola; las claves Modbus (MB-6, MB-11, MB-12, MB-15) sobreviven a exportar y reimportar |
| `test_button` | 4 | BOOT: reset a los 10 s (una vez), gesto de 2–10 s, botón presionado desde el arranque, rebotes, vuelta del reloj |
| `test_mbap` | 6 | MB-15: longitud de tramas MBAP, lecturas y escrituras reenviadas al esclavo simulado (FC01, 03, 05, 06, 15, 16), excepciones del propio gateway (01, 03, 0A) y del esclavo, unidad que no responde (0B), lista de clientes con redes CIDR |
| `test_dout` | 8 | Polaridad de 8 canales (activa en bajo, en alto y mixta), arranque del TCA9554 (latch OFF, POLARITY = 0 y luego CONFIG, también en arranque en caliente), DO/3 → pin bajo y relectura, errores de bus, relectura con POLARITY heredado invertido, reintento con backoff |

Los tests de host descargan Unity (v2.6.1) y cJSON (v1.7.19) con `FetchContent`, desde GitHub y **fijados por hash de commit**.

### 4.2 Interoperabilidad Sparkplug

```bash
mkdir -p /tmp/spb && build-host/spb_dump /tmp/spb && python3 idf/tools/spb_decode.py --check /tmp/spb
```

`spb_dump` genera NDEATH, NBIRTH, DBIRTH, DDATA y NDATA reales del motor. `spb_decode.py` los compila con `grpc_tools.protoc` a partir del `sparkplug_b.proto` oficial de eclipse/tahu (copiado sin cambios en `components/gw_core/proto/`) y los decodifica con **Google protobuf 7.36**.

Resultado: **8 payloads decodificados, 0 errores estructurales** (incluye un DBIRTH y un DDATA con UInt64, Int64, Double, String y un bit de registro). Además se decodificaron los 96 mensajes capturados en el E2E.

### 4.3 QA-3: punta a punta con el firmware real en QEMU

```bash
. $IDF_PATH/export.sh
python3 $IDF_PATH/tools/idf_tools.py install qemu-xtensa   # requiere libslirp0
python3 -m venv ~/e2e && ~/e2e/bin/pip install -r idf/tools/e2e/requirements.txt
E2E_PYTHON=~/e2e/bin/python idf/tools/e2e/run_e2e.sh
```

**Montaje:**

- QEMU `esp_develop_9.2.2`, máquina `esp32s3`, con el firmware compilado con el overlay `sdkconfig.qemu`.
- Ethernet `open_eth` sobre slirp.
- UART1 → socket → **pymodbus 3.8.6** como esclavo RTU, unidades 1 (`Meter1`: tags de §6, `Limit` float32 escribible y los tags MB-6 `Energy` uint64 ABCD, `Counter` int64 CDAB, `Flow` float64 DCBA, `Temp` int32 BADC, `Serial` string, `Alarm` y `Ready` bits de registro; `max_gap: 10`, `Energy` y `Serial` en escaneo lento de 3 s) y 2 (`Bulk`, 60 registros). Las unidades 3 a 5 (`Dead3`–`Dead5`) están en la configuración pero no existen en el bus: nunca responden.
- UART0 → socket, para el log y el shell (las DI se inyectan con su comando `di`).
- Modbus TCP (MB-15): slirp reenvía un puerto del host (`--mbtcp-port`, por defecto 15502) al 502 del gateway (`hostfwd`); un cliente pymodbus del harness lo usa. Las conexiones llegan desde 10.0.2.2, la única dirección de `allowed_clients` en la configuración del E2E.
- **mosquitto 2.0.18** privado (copia renombrada del binario), un respondedor SNTP propio en el UDP 123 y un receptor syslog UDP (`--syslog-port`, por defecto 15514). El harness solo envía señales a los procesos que arrancó él.
- **Brokers que se portan mal:** un servidor MQTT mínimo del harness en `--mqtt-port` + 1 acepta la conexión y la corta tras el CONNACK, y en `--mqtt-port` + 2 responde cada SUBSCRIBE con 0x80. (mosquitto no sirve para esto: su ACL nunca rechaza un SUBSCRIBE, solo filtra al entregar.)
- **Aislamiento de red.** El harness se mueve a un *network namespace* propio, con solo la interfaz de loopback (necesita `CAP_SYS_ADMIN`, por ejemplo root en un contenedor). Todos los puertos, también el UDP 123, son privados de la corrida, así que dos corridas en paralelo no chocan. Sin ese permiso (o con `--no-isolate`) usa el loopback del host con los puertos de `--mqtt-port`, `--modbus-port` y `--console-port` (o `E2E_MQTT_PORT`, `E2E_MODBUS_PORT`, `E2E_CONSOLE_PORT`); por defecto 18831, 15020 y 15555. Los argumentos extra de `run_e2e.sh` pasan a `e2e.py`.
- El harness escribe en la imagen bajo prueba su propio `config.json`, con el puerto MQTT elegido. Para los arranques de configuración genera variantes de la imagen con otra partición `storage` (con `littlefs-python`, el mismo que usa la build) o con la partición borrada. El límite de tamaño se lee de `build-qemu/sdkconfig` (`CONFIG_GW_CONFIG_MAX_BYTES`).
- "No arrancó nada" se comprueba con una señal positiva: la línea `nothing is started` y, 6 s después, la respuesta de `status` del shell (`nothing is running: no polling, no MQTT; recovery network and web UI only`). La ausencia de logs de arranque de la UART RS485 y de MQTT, de NBIRTH y de peticiones al esclavo Modbus queda como comprobación adicional.
- HTTP: slirp reenvía `127.0.0.1:18080` del host al puerto 80 del equipo (`--http-port`). El shell se consulta con un helper común que repite la pregunta hasta ver todas las líneas esperadas (#56). `--only failsafe,network,web,reset,main` corre solo algunos escenarios.

Configuración usada: `tools/e2e/fs/config.json` (keepalive de 10 s, syslog hacia el receptor del harness). `run_e2e.sh` pasa además el ELF y el python de IDF, para decodificar el *core dump* con `esp_coredump`.

Resultado: **89/89 comprobaciones OK**.

| Criterio | Resultado medido |
|---|---|
| CFG-1 / CFG-2: arranques con `config.json` de 131 073 B, vacío, con error de sintaxis, con error de esquema y con la partición borrada; y sin archivo | OK: en los cinco primeros no arranca la encuesta ni MQTT (ni UART RS485, 0 NBIRTH, 0 peticiones Modbus; solo la red de recuperación y la web), el LED pasa a error y el log ERROR da el motivo (`… 131073 bytes; el máximo es 131072`, `línea 5, columna 3`, `devices[0].tags[0].type`, `no montable`). Sin archivo: configuración embebida |
| §7.1 NBIRTH con seq = 0 y bdSeq; al matar la conexión TCP (SIGKILL a QEMU) el broker publica el NDEATH con el mismo bdSeq | OK: NBIRTH a 1,9 s del arranque; NDEATH con el bdSeq de la sesión |
| Reinicio del broker (SIGTERM, 9 s parado) | OK: mosquitto publica el will (NDEATH, bdSeq de la sesión) al cerrarse; backoff 1000 → 1866 → 3443 → 6860 ms con bdSeq nuevo en cada intento; NBIRTH (seq 0, bdSeq nuevo) y DBIRTH de los dos equipos (seq 1 y 2) |
| Broker congelado (SIGSTOP 20 s > 1,5 × keepalive) | OK: el gateway declara muerta la sesión por keepalive a los 10,0 s (`No PING_RESP`); tras SIGCONT llegan los will de la sesión muerta y del intento hecho durante el congelamiento, y luego NBIRTH con bdSeq nuevo y los dos DBIRTH; la sesión nueva sigue viva |
| §7.2 Esclavo apagado → DDEATH; encendido → DBIRTH | OK: DDEATH en 3,47 s (cota 5,8 s); DBIRTH 1,52 s después de volver (cota: tope de sondeo de 10 s más un ciclo) |
| MB-9 sondeo con backoff de los fuera de línea (tres unidades que no existen en el bus); #61 | OK: la unidad 3 recibe un solo intento por sondeo **desde el arranque**, sin ciclos completos, a 2,0 / 4,3 / 8,0 / 10,0 s; Meter1 hace 21 ciclos en sus primeros 19 s, intervalo máximo 1,68 s |
| MB-6 tipos nuevos | OK: DBIRTH con `Energy` UInt64 = 2^53 + 1 exacto, `Counter` Int64 = −(2^53 + 1) (CDAB), `Flow` Double (DCBA), `Temp` Int32 −123 456 (BADC), `Serial` String `SN-0042-ABCD`, `Alarm`/`Ready` Boolean (bits 3 y 15). DCMD `Counter` = 2^62 + 3 → **una** FC16 de 4 registros en orden CDAB y DDATA exacto; DCMD `Alarm` = false → FC03 y FC06 con 0x0101 (los otros bits quedan); DCMD `Serial` = `HELLO` → FC16 de 8 registros |
| MB-11 / #38 hueco por dispositivo | OK: Meter1 (`max_gap: 10`) pide exactamente coil 0, holding 10–21, 30–33, 34–37, 50–57, 60 e input 0–1, 40–46; Bulk (hueco 0 del firmware) sigue con una FC03 de 60 |
| MB-12 escaneo lento | OK: en 12 s, 12 ciclos rápidos (cada 1 s) y 4 lecturas de `Energy` y `Serial` a 3,04 / 3,00 / 3,01 s |
| MB-14 escritura entre bloques | OK: con respuestas de 120 ms, un DCMD enviado a mitad de un ciclo de Meter1 se escribe entre sus peticiones (1 antes, 4 después en el mismo ciclo), latencia 0,02–0,15 s desde la publicación |
| SPB-15 Scan Rate | OK: NBIRTH `Node Control/Scan Rate` UInt32 = 0; NCMD 3000 → NDATA 3000 y Meter1 cada 3,0 s; NCMD 10 → descartado con log, NDATA 3000; NCMD 0 → NDATA 0 y otra vez cada 1,0 s |
| MB-15 Modbus TCP | OK: cliente pymodbus por el puerto 502 lee holding 10 de la unidad 1 (= 123, el Setpoint) y escribe holding 200 = 4242 (0,11 s); unidad 3 muerta → excepción 0B; FC 0x2B → 01 del gateway sin pasar al bus. En el arranque de red aparte (`allowed_clients: ["192.0.2.0/24"]`), la conexión desde 10.0.2.2 se cierra sin respuesta y queda en el log |
| #69 contadores Modbus TCP | OK: tras esa corrida, `devices` da `Meter1 … \| tcp ok 2 timeout 0 …`, `Dead3 … \| tcp ok 0 timeout 3 …` y `modbus_tcp connections 2 accepted 0 rejected \| forwarded 3 \| gateway exceptions 01 1 03 0 0A 0 0B 1 \| other units …`; en el arranque con la lista restringida, `connections 0 accepted 1 rejected` |
| #68 resumen `config` | OK: `Meter1 unit 1, poll 1000 ms, slow 3000 ms, offline after 3, 11 tag(s) (9 fast, 2 slow), blocks max_gap 10`; `Bulk … blocks max_gap 0 (default)`; `modbus_tcp enabled, port 502, idle timeout 60 s, clients 10.0.2.2/32` |
| #65 *panic* con una respuesta Modbus en camino | OK: con la unidad 1 respondiendo 120 ms tarde, tres *panic* justo después de una petición suya; los tres arranques vuelven con NBIRTH y `Reset Reason` = `panic` |
| MQTT-2: broker que corta tras el CONNACK; SUBACK rechazado (0x80) | OK: el backoff sigue creciendo (1000 → 1696 → 3492 → 7681 ms). Con el SUBACK rechazado no hay NBIRTH, el gateway envía DISCONNECT y reintenta (1000 → 1861 → 3703 ms). El backoff vuelve al mínimo 10,1 s (un keepalive) después del NBIRTH de una sesión estable |
| REL-1: watchdog y *core dump* | OK: los births salen en 9 ms con 4 ms como máximo sin alimentar el TWDT; `panic` en el shell → "Core dump has been saved to flash", NBIRTH con `Reset Reason` = `panic` y `esp_coredump` decodifica el volcado (traza por `cmd_panic` y `abort`) |
| Afinidad de núcleos | OK: `tasks rs485_bus 1 sparkplug 0 tcpip 0 mqtt_task 0 emac_rx 0 sys_evt 0` |
| §7.3 DI → NDATA en menos de 100 ms; NCMD DO/3 = true → NDATA | OK: 23 ms (incluye 20 ms de debounce); NDATA alias 22 = true (pines simulados con la polaridad de `gw_bsp`) |
| §7.4 DCMD Setpoint = 12,3 → holding 10 = 123 por FC06 y DDATA | OK: el esclavo registró `(6, 10, [123])`; DDATA 12,3. Float32 `Limit` = 1234,5: **una sola** FC16 de 2 registros y DDATA 1234,5 |
| §7.5 NCMD Rebirth → NBIRTH (seq 0, mismo bdSeq) y todos los DBIRTH | OK |
| §7.6 Todos los payloads se decodifican con el proto oficial | OK: 96 mensajes, 0 errores |
| CFG-6 shell en la consola; REL-5 | OK: `devices` da `Meter1 unit 1 online cycles 2 failed 0 \| ok 6 …` y, con el esclavo apagado, `offline … failed 3 \| … timeout 9`; `config` da el resumen; `status` responde con la configuración válida (`config … ok`, `MQTT connected`, `Modbus 2/5 devices online`) y también con la configuración rechazada. NBIRTH `Node Info/Reset Reason` = `power-on` en el primer arranque y `software` tras NCMD Reboot |
| NET-4 sin radio y NET-7: configuración con `network.wifi` y `network.ipv6` (nodo `GW02`, arranque aparte) en la build de QEMU | OK: arranca, avisa `network.wifi ignored: Wi-Fi is not available in this build` y publica NBIRTH por Ethernet; el shell muestra `iface ethernet up 10.0.2.15 (active) \| wifi not configured` y el resumen de `network.wifi`. Con la configuración normal, `status` muestra Ethernet como única interfaz activa. IPv6: `fe80::5054:ff:fe12:3456` (link-local), `fec0::5054:ff:fe12:3456` (SLAAC con el RA de slirp) y `fec0::1234` (estática); `ping fec0::2`: 3 enviados, 3 respondidos. MQTT sobre IPv6 no se puede probar aquí (§6.6) |
| REL-4 syslog remoto (UDP al receptor del harness) | OK: `<134>1 2026-11-04T01:03:09.035Z GW01 edge - - [meta sequenceId="21"] Sparkplug session up (bdSeq=0)`: *facility* local0, HOSTNAME = nodo, APP-NAME = módulo, hora SNTP; 22 datagramas al primer NBIRTH, 0 mal formados; `status` da `syslog 10.0.2.2:15514 connected, sent 22, dropped 0, errors 0`. Con el broker caído llega `<132> … MQTT down; reconnect attempt 1 in 1000 ms`. 119 datagramas en 3 arranques, `sequenceId` sin huecos en cada uno |
| CFG-3 / CFG-4 recuperación (cinco archivos rechazados) | OK: la red arranca (DHCP), la web y `/api/status` responden con la contraseña de puesta en marcha (leída de la consola) y el motivo; 0 peticiones Modbus, 0 NBIRTH. Con el error de esquema, el documento se exporta, se corrige y se guarda por la API: el equipo reinicia y publica NBIRTH (`GW03`) |
| CFG-4 API REST (nodo `GW04`) | OK: sin credenciales o con otra contraseña → 401; `GET` con `mqtt.password` y `web.password` = `********`; `PUT` inválido → 400 `devices[0].tags[0].type`; sintaxis → 400 con línea 5, columna 9; `dry_run` → 200 sin guardar; 131 074 B → 413; `PUT` válido → NDEATH de `GW04`, NBIRTH de `GW05`, la misma contraseña sigue valiendo |
| CFG-3 interfaz web | OK: `/` (≈14 KB, HTML) y `/api/status` (modo, MQTT, `Meter1` en línea, `Voltage` = 230,5, Ethernet activa) |
| CFG-6 shell de configuración | OK: `config show` con 2 secretos enmascarados y ninguno en claro; `mbread 1 holding 10` = 50, `mbread 1 input 0 2` = 17254 32768 (230,5), unidad 9 → error; `rebirth` → NBIRTH seq 0 y DBIRTH seq 1 y 2; `config load` inválido → ruta; `config check` válido sin guardar; `config load` válido → NDEATH, NBIRTH de `GW06` |
| CFG-7 secretos | OK: al primer arranque `secrets moved from /cfg/config.json to NVS`; `NVS partition "nvs" is encrypted` (QEMU emula HMAC y eFuse); el shell dice `NVS encrypted (HMAC key in eFuse KEY5)`; ni la contraseña MQTT ni la web aparecen en claro en ningún lugar de la imagen de flash |
| CFG-5 reset de fábrica y NET-5 | OK: `boot 1` 3 s → gesto de punto de acceso (sin reset; la build de QEMU avisa que no hay radio); `boot 1` sostenido → aviso a los 2,1 s, reset a los 10,1 s, NDEATH ordenado, arranque en puesta en marcha (LED azul, 0 NBIRTH, ni la identidad de ejemplo); la contraseña web vieja → 401; la de puesta en marcha nueva abre `/api/status` (`commissioning`); una configuración cargada por la API arranca el equipo (`GW08`); en la partición no queda ningún bloque de la configuración vieja |
| NET-6 | OK: `mdns: gw01.local, _http._tcp port 80 ('Plant1/GW01')` (nombre derivado de `edge_node_id`). Las respuestas no se pueden probar con `open_eth` (§6.2.1) |
| Extras | OK: DDATA solo por excepción, solo alias, deadband 0,1 respetada; DCMD por alias (FC05) y DCMD sobre métrica de solo lectura descartado con log (sin "alias 0" si el comando venía por nombre); NCMD con un nombre de 80 caracteres después de `DO/1` descartado entero (DO/1 no cambia); bdSeq persistido en NVS tras un corte de energía; NCMD Reboot publica NDEATH QoS 1 y **un solo** NDEATH (will descartado); DBIRTH de 2445 B (mayor que el búfer de 2 KB de esp-mqtt); 60 registros leídos con una sola FC03; timestamps UTC vía SNTP; continuidad de seq |

**Lo que QEMU no cubre**, y por qué la build de emulador difiere (`sdkconfig.qemu`):

- **W5500.** No está emulado; se usa `open_eth`.
- **TCA9554, GPIO y RMT.** No están emulados; se usa `CONFIG_GW_IO_SIMULATED`: DO en RAM, DI con el comando `di` del shell y LED solo en el log.
- **Modo RS485 de la UART.** `uart_wait_tx_done` hace `assert` por `TX_DONE`; se usa UART normal.
- **Velocidades bajas.** El E2E corre a 9600 baudios; la espera del fin de transmisión a 1200 baudios solo tiene el test de host.
- **Watchdog de tareas.** QEMU corre con `wdt_disable` en los timers: el TWDT no puede dispararse. Se comprueba que la tarea Sparkplug lo alimenta entre publicaciones (log), no un enlace lento real.
- **TCA9554 ausente.** El expansor simulado siempre responde: el reintento y las DO `is_null` solo tienen tests de host.
- ***Core dump* y el otro núcleo.** La build de QEMU espera 1 s antes de reiniciar tras un *panic* (`CONFIG_ESP_SYSTEM_PANIC_REBOOT_DELAY_SECONDS`): un reinicio inmediato llegó a perder el final del volcado en la imagen de flash. Además, QEMU no parece detener el otro núcleo mientras corre el *panic handler*, como hace el chip: una tarea suya (el muestreo de DI a 1 kHz, por ejemplo) puede mover su pila entre las dos pasadas del volcado ELF, y entonces el checksum no cierra (2 de 5 corridas; en una, cabecera de 52 640 B y 21 088 B escritos). El E2E provoca hasta tres *panic*: todos deben guardar el volcado y reiniciar con `panic`, y al menos uno debe decodificarse.
- **PSRAM octal.** QEMU da *segfault*; se deshabilita.
- **Flash QIO.** Las lecturas de la app llegan desplazadas 2 bytes; se usa DIO.
- **IPv6 hacia el host.** slirp entrega las conexiones a `fec0::2` al `::1` del host y el kernel del contenedor no tiene IPv6: solo se prueban direcciones e ICMPv6.
- **UART1 tras un reinicio.** IDF reinicia la UART0 al salir pero no la UART1; el chip la limpia en `uart_driver_install()`, el modelo de QEMU no. Si un esclavo estaba respondiendo en el momento del reinicio, la línea de interrupción quedaba activa para siempre y el arranque siguiente se colgaba al instalar el driver (visto con gdb sobre el *gdbstub* de QEMU; 2 de 3 corridas del E2E tras NCMD Reboot o un *panic*). `gw_rs485` enmascara las interrupciones del puerto antes de todo reinicio: un *shutdown handler* para `esp_restart()` y un envoltorio de `esp_restart_noos()` (`-Wl,--wrap`) que cubre también el *panic*. 40 reinicios seguidos (20 NCMD Reboot, 20 *panic*) sin cuelgue. El E2E lo comprueba con tres *panic* lanzados justo cuando una respuesta Modbus está en camino (#65). Limpiar las interrupciones de la UART1 al arrancar, antes de `uart_driver_install()`, no alcanza en QEMU (se probó: la línea sigue activa con `INT_ENA` = 0); en la placa el driver ya reinicia la UART al instalarse. En la placa no hace falta, pero no molesta.
- **Wi-Fi.** QEMU no emula la radio; la build de emulador lleva `CONFIG_GW_WIFI=n` (y con eso `CONFIG_GW_SOFTAP=n`). El E2E comprueba solo que una configuración con `network.wifi` arranca, avisa que no hay Wi-Fi y sigue por Ethernet, y que los disparadores del punto de acceso llegan a `gw_ap`.
- **mDNS.** `open_eth` no tiene filtro multicast: el responder arranca pero no atiende la interfaz.
- **Botón BOOT.** No hay GPIO: `boot 1` / `boot 0` en el shell.

### 4.4 QA-2

`idf.py set-target esp32s3 build`: **0 warnings en toda la build**. Los componentes propios compilan con `-Wall -Wextra -Werror`.

### 4.5 QA-4: CI

`.github/workflows/ci.yml` corre en cada push y pull request, con tres jobs:

| Job | Qué hace |
|---|---|
| `host-tests` | Tests de host (CTest) y decodificación de los payloads de `spb_dump` con el proto oficial |
| `firmware` | `idf.py set-target esp32s3 build` en la imagen `espressif/idf:v6.1` fijada por digest; falla si aparece un `warning:`; publica los binarios y `idf.py size` como artefactos |
| `e2e` | Instala QEMU, mosquitto y los paquetes del harness y corre `run_e2e.sh`. Los contenedores de GitHub no permiten `unshare`, así que corre sobre el loopback del contenedor. Publica logs y payloads como artefactos |

## 5. Estado de los IDs del MVP

Leyenda de **Evidencia**:

- **H**: test de host.
- **Q**: E2E en QEMU.
- **C**: solo compilado, sin ejecutar.

| ID | Estado | Archivo principal | Evidencia / nota |
|---|---|---|---|
| NET-1 | Implementado | `components/gw_net/gw_net.c` | C para W5500; Q para la ruta esp_netif/eventos (con openeth). Reconexión del enlace por esp_eth |
| NET-2 | Implementado | `components/gw_net/gw_net.c` (`configure_ip`) | Q para DHCP; C para IP estática y DNS manual |
| NET-3 | Implementado | `components/gw_edge/gw_edge.c` (hostname → esp-mqtt/getaddrinfo) | C: el E2E usa una IP literal; DNS no ejercitado |
| TIME-1 | Implementado | `components/gw_net/gw_net.c`, `gw_edge.c` | Q: timestamps UTC a menos de 30 ms del host |
| MQTT-1 | Implementado | `components/gw_edge/gw_edge.c` | Q (host, puerto, client_id = edge_node_id; keepalive: broker congelado detectado en 1 × keepalive); usuario y contraseña C |
| MQTT-2 | Implementado | `gw_edge.c`, `gw_core/src/gw_backoff.c` | H + Q (reinicio del broker y broker congelado: NDEATH, bdSeq nuevo, NBIRTH y todos los DBIRTH; broker que corta tras el CONNACK: el backoff sigue creciendo) |
| MQTT-3 | Implementado | `gw_edge.c` (`prepare_attempt`) | Q |
| MQTT-7 | Implementado | `gw_edge.c`, `gw_bus.c` | Colas y búferes estáticos, `outbox.limit` y `SKIP_PUBLISH_IF_DISCONNECTED`. Sin prueba de estrés |
| SPB-1 | Implementado | `gw_core/src/spb.c`, `proto/` | H + interoperabilidad Python + Q |
| SPB-2 | Implementado | `gw_node.c`, `gw_edge.c` (`op_reboot`) | H + Q |
| SPB-3 | Implementado | `gw_storage/gw_storage.c`, `gw_edge.c` | Q (por intento y persistido) |
| SPB-4 | Implementado | `gw_core/src/gw_node.c` (`publish_nbirth`) | H + Q. Métricas de nodo: bdSeq, Rebirth, Reboot, `Node Info/Firmware`, `Hardware`, `Reset Reason`, `Node Control/Scan Rate` (alias 1–7), DI/1–8, DO/1–8 y `Counter/1`–`8` (IO-4, alias 30–37) |
| SPB-5 | Implementado | `gw_node.c` | H (vuelta 255 → 0) + Q (continuidad) |
| SPB-6 | Implementado | `gw_node.c` (`gw_node_device_changed`) | H + Q |
| SPB-7 | Implementado | `gw_node.c`, `gw_rbe.c` | H + Q |
| SPB-8 | Implementado | `gw_node.h` (mapa de alias) | H + Q |
| SPB-9 | Implementado | `gw_node.c` (`gw_node_rebirth`) | H + Q |
| SPB-10 | Implementado | `gw_node.c`, `gw_poll.c` (`gw_poller_write`) | H + Q. Comandos todo o nada: un NCMD/DCMD que no se decodifica entero no aplica ninguna métrica (H + Q); un DCMD cuyas escrituras no entran todas en la cola del bus tampoco (H) |
| SPB-11 | Implementado | `gw_node.c`, `gw_board/gw_io.c` | H + Q (DO simulada con la polaridad de la placa; TCA9554 C) |
| SPB-12 | Implementado | `gw_rbe.c` | H + Q |
| MB-1 | Implementado | `components/gw_rs485/gw_rs485.c` | Q a 9600 8N1; otras paridades C. La espera del fin de transmisión es proporcional a la trama (bits por carácter de la configuración + 20 ms): H a 1200 baudios con paridad |
| MB-2 | Implementado | `gw_rs485.c` (`UART_MODE_RS485_HALF_DUPLEX`) | **C**: QEMU no soporta el modo; la temporización de RTS/DE no está verificada. ISR de la UART en IRAM (`CONFIG_UART_ISR_IN_IRAM`) |
| MB-3 | Implementado | `gw_core/src/mb_rtu.c`, `gw_poll.c` | H (FC01–04) + Q (FC01, FC03, FC04) |
| MB-4 | Implementado | `mb_rtu.c`, `gw_poll.c` | H (las cuatro) + Q (FC05, FC06 y FC16 de 32 bits: una sola escritura y DDATA). FC15/FC16 de 16 bits con `CONFIG_GW_MB_FORCE_MULTI_WRITE`. NaN e infinitos rechazados también en coils |
| MB-5 | Implementado | `gw_core/src/gw_conv.c` | H + Q (float32, int16, uint16, bool; con MB-6 también int32 en `BADC`) |
| MB-7 | Implementado | `gw_conv.c` | H + Q |
| MB-8 | Implementado | `gw_core/src/mb_master.c` | H + Q |
| MB-9 | Implementado | `gw_devstate.c`, `gw_poll.c` | H (también un bloque sin respuesta mientras otro responde; sondeo con backoff de los fuera de línea y un equipo sano que mantiene su periodo junto a tres muertos) + Q (tres esclavos muertos sondeados a 2, 4, 8, 10 s sin atrasar al sano) |
| MB-10 | Implementado | `gw_poll.c` (`gw_poller_next`) | H + Q |
| MB-16 | Implementado | `components/gw_bus/gw_bus.c` | Diseño: una tarea y una cola. Las escrituras DCMD entran entre peticiones de un ciclo, siempre desde la misma tarea (MB-14) |
| IO-1 | Implementado | `components/gw_board/gw_io.c`, `gw_debounce.c`, `gw_bsp` | H (debounce y polaridad) + Q (pines simulados con la polaridad de `gw_bsp`); GPIO reales C |
| IO-2 | Implementado | `gw_core/src/gw_dout.c`, `gw_io.c`, `gw_bsp` | H (secuencia de arranque con POLARITY = 0, activa en bajo, mezclas, errores de bus, reintento con backoff y DO `is_null` mientras falta el expansor) + Q (registros simulados); I²C real C |
| IO-5 | Implementado | `components/gw_board/gw_status.c` | Q (estados en el log); RMT/WS2812 C; colores sin verificar |
| CFG-1 | Implementado | `gw_core/src/gw_config.c`, `gw_storage/gw_storage.c`, `main/main.c` | H (documento máximo de 256 tags, falta de memoria, rutas JSON, línea y columna, claves duplicadas) + Q (archivo demasiado grande, vacío, con error de sintaxis o de esquema y partición sin montar: nada arranca, LED de error, log ERROR con el motivo) |
| CFG-2 | Implementado | `gw_storage/gw_storage.c`, `main/CMakeLists.txt` | Q (LittleFS; respaldo embebido solo si falta el archivo) |
| FW-4 | Implementado | `idf/CMakeLists.txt` (`PROJECT_VER`) | Q (`0.1.0+g<hash>[.dirty]` en NBIRTH) |
| REL-1 | Implementado | `gw_bus.c`, `gw_edge.c`, `gw_io.c`, `sdkconfig.defaults` | Q: TWDT alimentado tras cada publicación (el log de los births informa el mayor intervalo sin alimentarlo), DI vigiladas como usuario del TWDT; un *panic* deja el *core dump* en flash, el equipo reinicia (`Node Info/Reset Reason` = `panic`) y el volcado se decodifica con `esp_coredump`. El disparo real del TWDT no se ejecutó: QEMU deshabilita el watchdog de los timers |
| REL-2 | **Parcial** | Todo `gw_*` | El código propio es estático y no asigna en el camino crítico; **lwIP y esp-mqtt asignan del heap internamente** (acotado, no estático) |
| REL-3 | Implementado | `esp_log` en todos los módulos | Q. Con REL-4 la consola sigue igual |
| QA-1 | Implementado | `test/host/` | 143 casos |
| QA-2 | Implementado | — | 0 warnings; tamaño en §3.2 |

**Fuera del MVP**, implementado total o parcialmente:

- **MB-6** Implementado: `uint64`, `int64`, `float64` y `string` (1–16 registros), órdenes `ABCD`/`CDAB`/`BADC`/`DCBA` (además de `big`/`little`) y `bit` 0–15 dentro de un registro `holding`/`input`, también escribible (lectura, modificación y escritura del registro). Los enteros de 64 bits viajan **exactos** de punta a punta: el valor interno es una unión (double, int64, uint64 o texto, según el tag), no un `double`; en Sparkplug van como UInt64/Int64 en `long_value`, `float64` como Double y `string` como String. El reporte por excepción compara los 64 bits (o un hash del texto). Evidencia: H (conversión en los cuatro órdenes, 2^53 + 1, límites, textos, esquema, encuesta, escritura de bit con relectura, DCMD exacto, DBIRTH/DDATA) + interoperabilidad Python + Q (DBIRTH con los siete tags nuevos en sus órdenes; DCMD int64 2^62 + 3 con una FC16 de 4 registros; bit y texto escritos).
- **MB-11** Implementado: los tags contiguos o cercanos del mismo área se leen en un bloque (hasta 125 registros o 2000 bits). Por dispositivo, `block_reads` (desactiva el agrupamiento) y `max_gap` (hueco máximo); sin ellas vale la opción de compilación (`CONFIG_GW_MB_BLOCK_READS`, `CONFIG_GW_MB_BLOCK_MAX_GAP`, por defecto agrupar con hueco 0). Evidencia: H (tres dispositivos con huecos distintos y uno sin agrupar; esquema) + Q (Bulk: 60 registros en una FC03 con el hueco 0 del firmware; Meter1 con `max_gap: 10`: holding 10–37 en una sola lectura).
- **MB-12** Implementado: `scan: slow` por tag y `slow_poll_ms` por dispositivo (por defecto 10 × `poll_ms`). El planificador arma bloques por clase y cada ciclo lee los rápidos, más los lentos cuando les toca. Con la detección de fuera de línea: los ciclos parciales cuentan para `offline_after`, y solo un ciclo completo (todos los bloques) pone el equipo en línea; fuera de línea, cada ciclo es completo. Evidencia: H (clases, bloques separados, ciclo completo al volver, lentos pendientes tras un fallo, equipo con solo tags lentos) + Q (Energy y Serial cada 3 s, el resto cada 1 s).
- **SPB-15** Implementado: `Node Control/Scan Rate` (UInt32, ms, alias 7) en el NBIRTH, escribible por NCMD. **Semántica: pisa, no escala.** 0 (el valor al arrancar) = cada dispositivo con su `poll_ms`; un valor de 50 a 3 600 000 ms reemplaza el `poll_ms` de **todos** los dispositivos, y también su `slow_poll_ms` si este es menor (MB-12). Un periodo más corto rige enseguida; uno más largo, después del sondeo ya programado. Los fuera de línea hacen su backoff a partir del periodo vigente. Un valor fuera de rango o no entero se descarta con log; en los dos casos un NDATA confirma el valor vigente. **No se persiste:** tras un reinicio vuelve a 0 (la configuración es la fuente de verdad: un valor guardado aparte haría que el equipo no se comporte como dice `config.json`, y cada NCMD gastaría flash). Evidencia: H (métrica, rango, confirmación, Int64 por alias, rebirth; planificación con el valor, vuelta a 0, backoff y escaneo lento) + Q (NCMD 3000 → Meter1 cada 3 s y NDATA; valor inválido rechazado; 0 → 1 s otra vez).
- **MB-15** Implementado: gateway Modbus TCP (`modbus_tcp`, **apagado por defecto**). Traduce MBAP → RTU (FC01–06, FC15, FC16) y pasa cada petición por la tarea dueña del bus: se atiende entre dos peticiones de la encuesta, después de las escrituras DCMD pendientes, una a la vez. El gateway responde él mismo las funciones que no reenvía (excepción 01), los datos no válidos (03), las unidades 0 y 248–255 (0A) y las unidades que no responden (0B). Control de acceso: `allowed_clients` (IPv4 o redes CIDR; vacío = cualquiera, con un warning al arrancar). Evidencia: H (tramas, excepciones, lecturas y escrituras contra el esclavo simulado, lista de clientes, esquema) + Q (un cliente pymodbus lee holding 10 y escribe holding 200 de la unidad 1 por el puerto 502, reenviado por slirp; la unidad 3 muerta da 0B; FC 0x2B da 01 sin tocar el bus; un cliente fuera de la lista se desconecta sin respuesta).
- **MB-13** Parcial: excepciones decodificadas y en el log; contadores por dispositivo (ciclos, ciclos fallidos e intentos ok / timeout / CRC / trama / excepción) en el shell y en el log de fuera de línea, no publicados por Sparkplug.
- **CFG-3** Implementado: interfaz web embebida (una página, sin recursos externos) con estado en vivo (`/api/status`: modo, interfaces, MQTT, dispositivos con contadores y valores, DI/DO, heap), formularios de red, MQTT, Sparkplug, RS485, credenciales y dispositivos/tags, editor JSON, validar, guardar, importar, exportar y reiniciar; Basic auth con hash PBKDF2. Arranca también con la configuración rechazada (recuperación) y en puesta en marcha. Evidencia: H (`gw_auth`: vectores SHA-256, HMAC RFC 4231, PBKDF2 RFC 7914, Basic) + Q (página, estado, 401, recuperación en los cinco casos de archivo rechazado, corrección por la API que arranca el equipo). Sin TLS (§6.2.1).
- **CFG-4** Implementado: API REST JSON (`GET/PUT/POST /api/config`, `?dry_run=1`, `?download=1`, `POST /api/restart`): validación con la ruta JSON o línea y columna del error, escritura atómica, reinicio ordenado (NDEATH), secretos enmascarados al exportar. H (`gw_cfgdoc`, campos de error, profundidad) + Q (401, export enmascarado, 400 con ruta, 400 con línea, dry run, 413, PUT válido → NDEATH y NBIRTH con la configuración nueva, el secreto se conserva con la máscara).
- **CFG-5** Implementado: reset de fábrica con BOOT 10 s (gestos en `gw_button`, host-tested), borrado de LittleFS completo y de NVS, modo puesta en marcha sin la identidad de ejemplo. H + Q (botón simulado: gesto de 3 s sin reset, reset a los 10 s, NDEATH, puesta en marcha, contraseña vieja rechazada, configuración cargada por la API, ningún bloque viejo en la partición). GPIO0 real, LED y buzzer real: C (los pitidos, Q en el log).
- **CFG-6** Implementado: shell con `status`, `devices`, `config` (resumen; por dispositivo, periodos rápido y lento, tags por clase de escaneo y lectura en bloque con su hueco, y una línea `modbus_tcp`, #68), `config show` (secretos enmascarados), `config load` / `config check` (pegar un JSON: validar, guardar de forma atómica y reiniciar), `restart`, `rebirth`, `mbread` (lectura Modbus por el dueño del bus) y `ping`; respuestas atómicas respecto del log (#56). Funciona con la configuración rechazada. Q (show, mbread de registro, float32 y equipo ausente, rebirth, load rechazado con ruta, check, load válido que reinicia). Sin contraseña (acceso físico); `CONFIG_GW_SHELL_WRITE_COMMANDS=n` lo deja de solo lectura.
- **CFG-7** Implementado: secretos fuera del JSON, en NVS; cifrada con la clave HMAC en eFuse KEY5 en la build de producción (`sdkconfig.production`) y en QEMU, sin cifrar en la build por defecto; migración de un archivo con secretos en claro (con borrado de la partición). Q (QEMU emula HMAC y eFuse: NVS cifrada; ningún secreto en claro en toda la imagen de flash tras la migración). La build de producción en placa y el quemado del eFuse: C (irreversible, §3.4).
- **NET-5** Implementado, **sin probar con radio**: punto de acceso `GW-Setup-XXXXXX` (WPA2, contraseña de puesta en marcha) sin configuración válida y con BOOT 2–10 s. Q (los dos disparadores llegan a `gw_ap`; la build de QEMU no tiene radio) + C (SoftAP, AP+STA).
- **NET-6** Implementado: `network.hostname` (o derivado de `edge_node_id`) en DHCP y mDNS con `_http._tcp`. H (esquema, derivación) + Q (el responder arranca con el nombre derivado) + C (respuestas mDNS: `open_eth` no tiene filtro multicast, §6.2.1).
- **MB-14** Implementado: la cola de escrituras DCMD se atiende antes de cada ciclo **y entre dos peticiones de un ciclo** (gancho `between_requests` de `gw_poll`): una escritura espera como mucho la transacción en curso, no el resto del ciclo ni los de otros equipos. Si escribe un tag del equipo que se está encuestando, el valor confirmado también reemplaza la copia de trabajo del ciclo (no se publica después el valor viejo leído antes). Evidencia: H (ciclo de 10 peticiones: la escritura sale en el siguiente límite, latencia de una transacción, valor confirmado publicado) + Q (con respuestas de 120 ms, un DCMD a mitad de ciclo se escribe entre peticiones del mismo ciclo).
- **IO-3** Implementado: estado seguro por salida (`local_io.do_failsafe`: `off`, `on` u `hold`), al reiniciar y tras `local_io.failsafe_s` sin sesión Sparkplug (SPEC §6.1). Antes de cargar la configuración, todas a OFF salvo las `hold` de la última configuración válida (máscara en NVS), que conservan el latch del TCA9554 si el reinicio fue en caliente (sus pines ya eran salidas); con la configuración rechazada, todas a OFF. Una sola tarea (`local_io`) escribe el expansor. Evidencia: H (`gw_dout`: lectura de CONFIG y OUTPUT antes de escribir, latch conservado solo con pines de salida, arranque en frío, lectura fallida; `gw_localio`: estado de arranque, disparo una vez a los N s desde el arranque o la pérdida, rearme, vuelta del reloj; esquema) + Q (NBIRTH con `on` encendida al arrancar; broker caído 9 s con `failsafe_s` 5: disparo a los 5 s, NBIRTH siguiente con los estados seguros; `hold` conservada tras NCMD Reboot y apagada tras un corte, con el expansor simulado en memoria RTC) + C (I²C real).
- **IO-6** Implementado: buzzer en GPIO46 (LEDC, configurado recién en `app_main`: es pin de *strapping*) con alarmas configurables (`local_io.buzzer`: `config_error`, `mqtt_down`, `device_offline`, con `alarm_after_s`) y los pitidos del botón BOOT: 2 al confirmar el reset de fábrica (CFG-5; `gw_buzzer_beep_wait`, antes del borrado) y 1 para el punto de acceso. Una sola tarea maneja el buzzer; el botón solo le pide pitidos. Evidencia: H (`gw_buzz`: pitidos pedidos, demora, prioridad, periodo de 10 s, pedido antes que la ráfaga de alarma, buzzer silencioso; esquema) + Q (en el log de la build de QEMU: `device offline` 5 s después del arranque, `MQTT down` 5 s después de perder la sesión y de vuelta a `device offline`, `configuration error` en recuperación según NVS, 1 y 2 pitidos del botón) + C (LEDC y GPIO46 reales).
- **IO-4** Implementado: contador de activaciones por DI (flancos del debounce, hasta unos 25 Hz con 20 ms), métricas `Counter/1`…`Counter/8` UInt64 en el NBIRTH y en el NDATA del cambio. Se guardan en NVS cada `local_io.counter_save_s` (600 s por defecto) si cambiaron, y antes de cada reinicio ordenado (*shutdown handler*); se restauran al arrancar. Sin PCNT ni la RAM del PCF85063 (un solo byte). Evidencia: H (conteo de flancos, solo activaciones, UInt64 exacto por encima de 2^53, NBIRTH y NDATA, calendario de guardado, esquema) + Q (NBIRTH con 8 contadores, NDATA con DI/1 y `Counter/1` = 1, guardado periódico que sobrevive a un corte, guardado al NCMD Reboot).
- **IO-7** Implementado: reglas locales DI→DO (`local_io.rules`, hasta 8, una por DO): `follow`, `invert` y `toggle` (cuenta las activaciones con los contadores de IO-4, así que no pierde ninguna entre dos evaluaciones). Las evalúa la tarea `local_io`, única dueña del TCA9554, despertada por el muestreo de las DI: funcionan sin red. **Prioridades:** la DO con regla es de la regla (NCMD rechazado con log y NDATA del estado real; el estado seguro por pérdida de MQTT no la toca); al reiniciar arranca en su `do_failsafe` (`on` se rechaza al validar) hasta la primera evaluación. Evidencia: H (`follow`/`invert`/`toggle`, pulsos restaurados que no invierten, número par de activaciones, estado seguro sin las DO de reglas; esquema: DO repetida, `on` con regla, rangos) + Q (`di 2 1` → DO/5 en NDATA sin comando, NCMD sobre DO/5 rechazado, dos activaciones de DI/3 → DO/6 encendida y apagada, con el broker caído DI/3 enciende DO/6 y el estado seguro deja DO/5 y DO/6 como están) + C (I²C real).
- **NET-4** Implementado, **sin probar con radio**: Wi-Fi STA como respaldo (`role: backup`, Ethernet gana la ruta) o primaria (`role: primary`), con *failback* por histéresis (`failback_s`) y sesión MQTT nueva (bdSeq nuevo, NBIRTH) al cambiar de interfaz. Evidencia: H (`gw_netsel`: failover, failback, rebotes, solo Ethernet; esquema `network.wifi`) + Q (configuración con Wi-Fi en la build sin radio: aviso y Ethernet como antes; shell `status` con la interfaz activa) + C (driver Wi-Fi, conmutación real, atadura del socket).
- **NET-7** Implementado: IPv6 opcional (`network.ipv6`) en todas las interfaces: link-local en cada subida del enlace, SLAAC con los RA (`CONFIG_LWIP_IPV6_AUTOCONFIG`, activada por interfaz solo si la configuración lo pide), dirección estática en Ethernet y DNS IPv6. Una interfaz con solo IPv6 global cuenta como utilizable. Evidencia: H (literales IPv6, esquema) + Q (link-local, SLAAC sobre `fec0::/64` de slirp, estática `fec0::1234`, `ping fec0::2` respondido) + C (MQTT, SNTP y DNS sobre IPv6; ver §6.6).
- **REL-4** Implementado (syslog; logs por MQTT no): cada línea de `esp_log` se copia, además de a la consola, a un servidor syslog RFC 5424 por UDP (`syslog.host`, `port`, `level`). Quien loguea nunca espera: la línea se formatea solo si el candado de formato está libre (*try-lock*) y entra en un anillo de `CONFIG_GW_SYSLOG_SLOTS` líneas (en PSRAM si hay); si no, se descarta y se cuenta, y el servidor recibe después un aviso con la cantidad. Las líneas de la propia tarea emisora no se reenvían (sin recursión). Evidencia: H (parseo de líneas de `esp_log`, formato RFC 5424, timestamps, anillo lleno, esquema) + Q (líneas con PRI, HOSTNAME, APP-NAME y timestamp SNTP; warning con el broker caído; `sequenceId` sin huecos en cada arranque; contadores en `status`).
- **REL-5** Implementado: `esp_reset_reason()` va al log de arranque y al NBIRTH como `Node Info/Reset Reason` (String, alias 6). H + Q (`power-on` en el primer arranque, `software` tras NCMD Reboot).
- **REL-6**: medido en QEMU.
- **QA-3** Parcial: QEMU, sin hardware.
- **QA-4** Implementado: `.github/workflows/ci.yml` (§4.5).

## 6. Desvíos y limitaciones conocidas (sin maquillaje)

### 6.1 Sin hardware

Nunca se ejecutaron:

- el driver W5500 y su SPI;
- el modo RS485 por hardware (temporización de RTS/DE a 115200 incluida). La ISR de la UART está en IRAM (`CONFIG_UART_ISR_IN_IRAM`) para que el DE se libere a tiempo aunque la caché esté deshabilitada (por ejemplo, durante el `nvs_commit` del bdSeq); falta verificarlo con osciloscopio;
- el TCA9554 real;
- la polaridad real de las DI;
- el orden de color del LED;
- la PSRAM octal;
- el arranque con flash QIO;
- el Wi-Fi: asociación, reconexión, conmutación entre interfaces con dos enlaces reales y la atadura del socket MQTT (`SO_BINDTODEVICE`).

La build de QEMU difiere de la de placa en esos puntos (`sdkconfig.qemu`).

### 6.2 Configuración

- **Configuración inválida o ilegible = ni encuesta ni MQTT.** La SPEC pide "no arrancar la encuesta". Arrancan solo la red (la de la última configuración válida, guardada en NVS cuando cambia, o DHCP por Ethernet), la web, el shell y el punto de acceso, para corregirla sin regrabar. Si la red de la última configuración válida ya no sirve (por ejemplo, una IP estática de otra planta), queda el punto de acceso o el shell.
- **Respaldo embebido solo si falta el archivo.** Un equipo grabado sin la partición `storage` (por ejemplo, solo la app) no monta LittleFS y queda detenido: es deliberado, para no arrancar nunca con la identidad Sparkplug del ejemplo.
- **Validación estricta y decisiones propias:**
  - claves desconocidas o duplicadas = error;
  - `coil`/`discrete` ⇒ `bool`;
  - `scale` ≠ 0;
  - `bool` sin `scale`/`offset`;
  - nombres de tag y dispositivo ≤ 39 caracteres;
  - `group_id`/`edge_node_id` ≤ 32 caracteres, sin `/ + #`;
  - rangos elegidos aquí: `response_timeout_ms` 10–5000, `retries` 0–5, `poll_ms` ≥ 50, `keepalive_s` 5–3600, `inter_frame_delay_ms` ≤ 1000;
  - MB-6: `bool` en `holding`/`input` exige `bit`; `bit` no admite otro `word_order`; `length` solo con `string`; `string` sin `scale`, `offset` ni `deadband`.

  La implementación Zephyr puede ser más laxa: hay que acordarlo.
- **`primary_host_id`** se valida y se ignora con un warning (SPB-13 no está en el MVP).
- **`units`** se valida y se guarda, pero no se publica (SPB-16).

### 6.2.1 Web, API, shell y secretos (CFG-3 a CFG-7, NET-5, NET-6)

- **Sin TLS.** La web y la API van por HTTP: usuario y contraseña viajan en Basic auth (base64, en claro) por la red. Usar solo en redes de planta confiables o con el punto de acceso. `esp_https_server` queda pendiente (necesita certificado por equipo).
- **Sin límite de intentos.** Cada intento fallido cuesta un PBKDF2 (decenas de ms) y queda en el log (`wrong credentials`), pero no hay bloqueo. Solo se recuerda el último encabezado `Authorization` aceptado (su SHA-256), para no recalcular PBKDF2 en cada pedido.
- **En recuperación la contraseña web guardada vale con cualquier usuario:** el usuario vive en el documento rechazado.
- El shell no pide contraseña: llegar a la consola exige acceso físico (USB-C), el mismo que da el botón BOOT y la flash. `CONFIG_GW_SHELL_WRITE_COMMANDS=n` deja una consola de solo lectura.
- `config load` lee el documento pegado hasta que cierra su objeto exterior; abandona tras 60 s sin datos. La lectura de la consola no tiene eco.
- `mbread` va por la cola del bus (dueño único, MB-16): espera a que termine la transacción en curso; un equipo inexistente da `timeout` o `framing error` (según lo que conteste el resto del bus).
- **El punto de acceso (NET-5) solo está compilado** (QEMU no tiene radio). No hay portal cautivo: hay que abrir `http://192.168.4.1/`. Con Wi-Fi STA configurado el driver pasa a AP+STA y el AP queda en el canal del STA.
- **mDNS (NET-6) solo está compilado en cuanto a las respuestas:** en QEMU el responder arranca, pero `open_eth` no tiene filtro multicast y el responder deja la interfaz afuera. El W5500 sí tiene filtro (`add_mac_filter`), sin verificar en placa.
- El reset de fábrica y la migración borran la partición LittleFS de 1 MB completa: en flash real son algunos segundos (en pasos de 64 KB).
- Los secretos en NVS (cifrada) se descifran a RAM al arrancar: un volcado de memoria (*core dump* en flash) los puede contener.
- **Buzzer (IO-6):** LEDC a 2,7 kHz sin verificar el tipo de buzzer ni su frecuencia de resonancia (el demo usa 1 kHz). No se puede silenciar una alarma mientras dura su condición (el botón BOOT ya tiene sus gestos). La alarma `config_error` depende de lo que pidió la última configuración válida (NVS): en un equipo que nunca tuvo una, no suena. Durante un borrado largo de flash (reset de fábrica) la tarea del buzzer se detiene: por eso los dos pitidos del reset suenan completos **antes** de borrar.
- **QEMU y los reinicios.** En el emulador `esp_restart` no reinicia la UART1 ni la matriz de interrupciones: si llega una respuesta Modbus durante el reinicio, el arranque siguiente se cuelga en `uart_driver_install` (tormenta de interrupciones). Los reinicios ordenados (REST, shell, NCMD Reboot, reset de fábrica) silencian el bus antes (`gw_bus_quiesce`), lo que además evita cortar una trama en la placa.

### 6.3 Comportamientos Sparkplug elegidos

- Una lectura fallida (excepción) se publica como `is_null` en DDATA. No hay calidad (SPB-16).
- Un dispositivo arranca fuera de línea y solo se publica su DBIRTH tras la primera respuesta.
- **Un ciclo falla si alguna petición queda sin respuesta tras los reintentos** (una excepción Modbus cuenta como respuesta). El ciclo se corta ahí, sin gastar el timeout en los bloques restantes, y los ciclos fallidos cuentan para `offline_after`. Así el DDEATH llega en `offline_after` ciclos aunque los demás bloques del equipo respondan.
- Los tags de los bloques no leídos **conservan el valor y el timestamp de su última lectura**: no se reportan como frescos. Si la falta persiste, el DDEATH los invalida a todos. Un equipo pasa (o vuelve) a estar en línea solo tras un ciclo completo, así que su DBIRTH nunca lleva valores que no se leyeron.
- **Sondeo con backoff de los dispositivos fuera de línea.** Un equipo fuera de línea ya no ocupa el bus como uno sano: cada sondeo manda **solo la primera petición del ciclo, con un único intento** (sin reintentos), y el siguiente se programa a 2, 4, 8… × `poll_ms`, con tope en `CONFIG_GW_MB_OFFLINE_PROBE_MAX_MS` (10 s por defecto; nunca menos que `poll_ms`). Si responde, el ciclo sigue completo en esa misma vuelta. Está fuera de línea:
  - un equipo que estuvo en línea, tras `offline_after` ciclos fallidos seguidos (MB-9);
  - **un equipo que nunca respondió desde el arranque, desde su primer ciclo** (#61): para Sparkplug ya está fuera de línea (no tuvo DBIRTH), así que no recibe ciclos completos con reintentos y el backoff empieza en su primer fallo. Antes, con varios equipos ausentes, el bus iba saturado los primeros segundos (Meter1 llegaba a 2 s de intervalo). El costo: si la primera petición se pierde (ruido, esclavo que arranca más lento que el gateway), su DBIRTH llega 2, 4… × `poll_ms` después en lugar de un ciclo después.

  **Vuelta a línea acotada:** el DBIRTH llega como mucho un intervalo tope más un ciclo después de que el equipo vuelve a responder (≤ 10 s + ciclo por defecto). Con los valores por defecto un equipo muerto ocupa el bus 200 ms cada 10 s, en lugar de 600 ms por segundo.
- Un DCMD a un dispositivo fuera de línea se descarta, no se encola.
- Las escrituras se reintentan como cualquier transacción. FC05/06/15/16 son idempotentes, pero un esclavo que ejecuta y no responde recibe la escritura dos veces.
- **Rebirth espontáneo:** si el primer NBIRTH salió antes de que sincronizara SNTP (timestamps de 1970), se reemite al sincronizar. Esta rama no se ejercitó en el E2E.

- **MB-6:**
  - La escritura de un `bit` lee el registro y lo reescribe entero. Nada del gateway se mete entre la lectura y la escritura (las dos pasan por la tarea dueña del bus), pero si el propio esclavo cambia otro bit de ese registro en ese intervalo, el cambio se pierde. Los esclavos con FC22 (*mask write*) no se aprovechan.
  - Los `string` llegan hasta 32 caracteres (16 registros) y solo ASCII imprimible (lo demás sale como `?`): no hay UTF-8 ni otras codificaciones. El texto viaja como String de Sparkplug; para el reporte por excepción se compara un hash FNV-1a de 64 bits (una colisión ocultaría un cambio; probabilidad despreciable).
  - Con `scale`/`offset`, un `int64`/`uint64` pasa a Double y pierde la exactitud por encima de 2^53, como cualquier valor escalado.
  - **Memoria:** el valor de cada tag ocupa ahora 56 B (antes 24) para alojar el texto. Son unos 13 KB más de `.bss` interna (imagen de proceso de 256 tags, copia de trabajo de la encuesta y la del shell; §3.2) y 2 KB más en el motor Sparkplug (PSRAM si la hay).

- **MB-15 (Modbus TCP):**
  - **Sin autenticación ni cifrado:** cualquiera que llegue al puerto y esté en `allowed_clients` lee y escribe todos los esclavos del bus, también registros que el gateway no encuesta. La lista filtra por dirección de origen (falsificable en la misma red). Por eso viene apagado.
  - Solo IPv4: con IPv6 activo (NET-7) el servidor no escucha en IPv6.
  - Una transacción a la vez para todos los clientes; mientras tanto los demás esperan. Una petición espera como mucho la transacción en curso y las escrituras DCMD pendientes; si el bus no la ejecutó en 8 s (`CONFIG_GW_MBTCP_RESPONSE_WAIT_MS`), el cliente recibe 0B.
  - Sin FC07, FC08, FC17, FC22, FC23 ni FC43: excepción 01. Tampoco *broadcast* (unidad 0).
  - **Contadores (#69):** cada dispositivo cuenta aparte los intentos de las peticiones TCP reenviadas a su unidad (`devices`: `| tcp ok … timeout … crc … frame … exception …`), y la pasarela cuenta conexiones aceptadas y rechazadas, peticiones reenviadas, las excepciones que respondió ella misma (01, 03, 0A, 0B) y los intentos hacia unidades sin dispositivo configurado (línea `modbus_tcp` de `devices`). No se publican por Sparkplug (SPB-17 no está).
  - Memoria: unos 5 KB de `.bss` (pila de 4 KB, búferes de 2 clientes y la transacción), aunque esté apagado.

### 6.4 Reloj

Sin SNTP no hay hora válida: no hay RTC (TIME-2). Antes de la primera conexión se esperan hasta 3 s a SNTP (`CONFIG_GW_SNTP_WAIT_MS`). Se desactivó el retardo aleatorio de arranque de SNTP de lwIP, porque en una de las corridas empujó la sincronización más allá del NBIRTH.

### 6.5 Broker lento y límites de comandos

- Un broker lento bloquea la tarea Sparkplug mientras dura cada publicación (cada escritura de socket espera hasta 5 s, `network.timeout_ms`). La memoria no crece: los cambios se **coalescen** y se publica el último valor; los intermedios se pierden. No es store & forward.
- **Watchdog con un enlace lento.** La tarea Sparkplug alimenta el TWDT después de cada publicación, así que un rebirth (NBIRTH y hasta 16 DBIRTH) puede durar más de 15 s sin reiniciar el equipo. Lo que debe entrar en los 15 s es **una** publicación: el DBIRTH más grande (unos 6 KB) en un enlace de menos de unos 400 B/s todavía dispararía el watchdog. Tras cada NBIRTH el log informa cuánto tardaron los births y el mayor intervalo sin alimentar el watchdog. QEMU deshabilita el watchdog de los timers (`wdt_disable`), así que el caso del enlace lento no se ejecutó.
- Los comandos de más de 1024 bytes, o fragmentados por esp-mqtt, se descartan con log.
- Un NCMD o DCMD que nanopb no puede decodificar entero (por ejemplo, un nombre o un string de más de 63 caracteres) se descarta **completo**, con log: se decodifica una vez para validarlo y otra para aplicarlo.
- La cola de escrituras hacia el bus tiene 8 lugares (`CONFIG_GW_BUS_WRITE_QUEUE_LEN`). Un DCMD cuyas escrituras no entran **todas** se descarta **entero**, con log (`DCMD for 'Meter1': 3 writes but the bus queue has room for 2: nothing applied`): nunca queda aplicado a medias (#60). Las métricas desconocidas o de solo lectura no cuentan: se descartan solas, como antes.

### 6.6 Red: Wi-Fi, conmutación (NET-4) e IPv6 (NET-7)

- **La falla se detecta por enlace o dirección, no por alcance.** Si el cable sigue enchufado pero el switch o el router de Ethernet dejan de llevar al broker, no hay conmutación: la sesión cae por keepalive y se reintenta por la misma interfaz. No hay sondeo del broker por cada interfaz.
- **Arranque con las dos interfaces:** si Wi-Fi obtiene IP antes que Ethernet (el W5500 tarda 1–3 s en negociar), la primera sesión va por Wi-Fi y vuelve a Ethernet a los `failback_s`: hay una sesión extra (un NDEATH y otro NBIRTH) en el arranque.
- **Cambio de interfaz con la vieja muerta:** el NDEATH explícito no llega y el broker publica el *will* recién cuando vence su keepalive (1,5 × `keepalive_s`), con el bdSeq de la sesión vieja y quizás después del NBIRTH nuevo. Un host Sparkplug lo descarta por bdSeq. Si el broker publica el *will* al tomar la sesión el cliente nuevo (mismo client_id), pasa lo mismo.
- `esp_mqtt_client_destroy` puede bloquear la tarea Sparkplug hasta el timeout de red (5 s) si el cliente estaba conectando; queda por debajo del watchdog de 15 s.
- Las credenciales Wi-Fi no se copian a la NVS del driver (`WIFI_STORAGE_RAM`); la contraseña vive en los secretos de NVS cifrada (CFG-7), no en `config.json`.
- La reconexión Wi-Fi usa backoff con jitter de 1 a 30 s. Ahorro de energía del radio desactivado (`WIFI_PS_NONE`) por la latencia de los comandos.
- **MQTT sobre IPv6 no se probó de punta a punta.** slirp reenvía las conexiones a `fec0::2` al `::1` del host, y el kernel del contenedor de prueba no tiene IPv6 (`AF_INET6` no soportado). El E2E prueba direcciones (link-local, SLAAC, estática) y alcance ICMPv6 al host virtual de slirp. esp-mqtt resuelve el host con `getaddrinfo` y acepta literales IPv6; SNTP y DNS sobre IPv6 quedan solo compilados.
- IPv6: sin DHCPv6 ni *gateway* estático (el router sale de los RA; sin RA, una dirección estática solo alcanza su /64). La dirección SLAAC usa EUI-64 (deja ver la MAC; lwIP no tiene direcciones temporales). La dirección estática es solo de Ethernet. El shell muestra las direcciones vistas desde la última subida del enlace; lwIP no avisa cuando una vence.
- La IP estática de Wi-Fi tiene las mismas reglas que la de Ethernet. Con las dos interfaces en la misma subred, el tráfico que no es MQTT (SNTP, DNS) sale por la que elija lwIP; MQTT va atado a la activa.

### 6.7 Syslog remoto (REL-4)

- **Solo UDP, sin confirmación.** Un datagrama perdido en la red no se reintenta. El `sequenceId` lo numera el emisor al enviar: un hueco en el servidor es pérdida en la red; lo que se descartó en el equipo llega como un aviso `N log line(s) not forwarded`.
- **Logs por MQTT no están.** Mezclarían tráfico que no es Sparkplug en la sesión del nodo y la cola de publicación; syslog sigue andando con el broker caído, que es cuando más se necesita el log.
- Se descarta una línea si otra tarea está formateando en ese mismo instante (el candado no espera) o si el anillo está lleno (sin red, servidor inalcanzable). Una línea de más de 256 bytes se trunca; el mensaje, a 199 caracteres.
- Las líneas anteriores a cargar la configuración (arranque, motivo de reinicio) no se reenvían: el destino está en la configuración.
- Sin TLS (RFC 5425). El servidor se resuelve al abrir el socket, recién con una interfaz activa, y otra vez tras un error de envío; si no resuelve, se reintenta cada 5 s.
- Las líneas de `ESP_EARLY_LOG` y las de interrupciones no pasan por `esp_log_set_vprintf` y quedan solo en la consola.

### 6.8 Persistencia y alias

- El bdSeq se escribe en NVS en cada intento de conexión. Desgaste estimado: décadas con un intento por minuto.
- Los alias dependen del orden de los tags en la configuración.

### 6.9 DI y DO

- Una DI que cambia más rápido que el debounce se pierde, y su pulso no cuenta (IO-4): sin PCNT, el contador ve hasta unos 25 Hz con 20 ms de debounce.
- **Contadores (IO-4):** un corte de alimentación, un *panic* o el watchdog pierden lo contado desde el último guardado (como mucho `counter_save_s`, 10 min por defecto). Guardar más seguido gasta la flash: con 60 s, unas 1 440 escrituras de 64 B por día, décadas con el *wear leveling* de NVS. No hay NCMD para ponerlos a 0; el reset de fábrica los borra con NVS. Un reinicio ordenado los guarda en el *shutdown handler*, salvo después de un reset de fábrica (NVS ya no acepta escrituras de estado).
- El timestamp de NDATA es el del lote de cambios, no el de cada bit.
- **Arranque de las DO (IO-3).** En cada arranque (en frío, en caliente, por watchdog o por panic), **antes de cargar la configuración**, las DO pasan a OFF, salvo las `hold` de la última configuración válida: esa máscara se guarda en NVS al aceptar una configuración y se lee al arrancar, así que NVS se inicializa antes que las salidas. El TCA9554 no se reinicia con el ESP32 y conserva el latch; por eso el firmware lee CONFIG y OUTPUT y reescribe el latch (OFF, o el nivel conservado de una `hold` cuyo pin ya era salida) antes de pasar los pines a salida. También pone a 0 el registro de polaridad (0x02), que el expansor conserva y que invertiría la relectura si otro firmware lo dejó distinto de 0. Con la configuración válida, las `on` se encienden; con la configuración rechazada o en puesta en marcha, también las `hold` pasan a OFF (hasta entonces conservaron su estado: lo que tarda cargar la configuración, del orden de décimas de segundo; varios segundos si hay una migración de secretos).
- **Un cambio de `hold` rige desde el arranque siguiente al que lo cargó:** el primer paso del arranque usa la máscara de la configuración anterior.
- **Estado seguro por pérdida de MQTT:** cuenta la sesión Sparkplug (NBIRTH publicado), no el enlace. Se dispara una vez por corte; al volver la sesión las DO no vuelven solas a su estado anterior. Un NCMD no puede llegar sin sesión, así que no hay conflicto con él. No toca las DO de las reglas locales (IO-7).
- **Expansor que no responde.** Si el TCA9554 no contesta al arrancar (por ejemplo, un glitch de I²C), las DO se publican como **`is_null`** en el NBIRTH (el latch puede conservar salidas encendidas de antes de un reinicio en caliente: nunca se informan como apagadas sin saberlo) y los NCMD sobre ellas se rechazan. La tarea `local_io` reintenta la inicialización a 1, 2, 4… hasta 60 s; cuando el expansor responde, las DO toman su estado de arranque (IO-3) y un NDATA publica sus 8 valores. En recuperación (configuración rechazada) no hay reintentos. Solo compilado: QEMU no emula el TCA9554 (lógica con tests de host).
- **Reglas locales (IO-7):** no hay más lógica que DO = DI, DO = no DI y conmutación por flanco: ni temporizadores, ni combinaciones de varias DI, ni reglas hacia los esclavos Modbus. Una DO con regla no se puede mandar por NCMD ni tiene estado seguro por pérdida de MQTT. La latencia es la del debounce más el despertar de la tarea `local_io` y una escritura I²C (unos milisegundos en la placa, sin medir); con el expansor caído las reglas esperan a que vuelva.
- La polaridad de las DO (activa en bajo) se dedujo del demo y de ESPHome, sin esquemático ni placa. Si la medición dice lo contrario, basta con `CONFIG_GW_DO_ACTIVE_LOW=n`.

### 6.10 Pruebas y entorno

- La versión de firmware lleva `.dirty` hasta que el coordinador haga el commit.
- `test_poll` (`test_unanswered_block_fails_the_cycle`) falló una vez en 7 corridas con `Expected …756 Was …757`: el timestamp de un tag no leído cambió 1 ms. Parece depender del reloj de pared del host (un cambio de milisegundo durante el ciclo); no es de esta rama y no se tocó.
- Los tests de host necesitan red la primera vez (FetchContent de Unity y cJSON).
- Mientras corría el E2E, otro proceso del host compartido ocupó el UDP 123 y terminó el mosquitto por nombre. Por eso el harness usa una copia renombrada del broker y corre en su propio *network namespace* (§4.3). Sin aislamiento, si el UDP 123 está ocupado el check TIME-1 falla: exige que haya respondido el SNTP del harness.
- **pymodbus se fijó en 3.8.6**: la 3.16 cambió el API del datastore (SimDevice).
- `idf.py` no acepta `-j`: la build usó el paralelismo por defecto de ninja.
- Hay dos componentes de terceros: nanopb (vendorizado) y `joltwallet/littlefs`. El paquete `livekit/nanopb` del registro se descartó por un defecto de ABI. `espressif/esp_io_expander_tca9554` se descartó: carga el latch con 0xFF, que con las DO activas en bajo es justamente OFF, pero no permite probar en host la secuencia ni la polaridad (`gw_dout`). La viabilidad (`docs/feasibility-idf.md`, IO-2) lo descartaba suponiendo, por error, DO activas en alto.
