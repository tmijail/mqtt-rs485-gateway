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
| `gw_core` | **Lógica pura en C, sin IDF; se prueba en el host:**<br>• `gw_config`: parseo y validación JSON §6.<br>• `gw_dout`: secuencia de arranque del TCA9554 y polaridad de las E/S.<br>• `mb_rtu`: CRC y tramas FC01–06/15/16.<br>• `mb_master`: transacciones, t3,5, timeout y reintentos.<br>• `gw_conv`: tipos, escala, orden de palabras y datatype Sparkplug.<br>• `gw_plan`: lecturas en bloque.<br>• `gw_poll`: encuesta, fuera de línea y escrituras.<br>• `gw_rbe`: reporte por excepción y deadband.<br>• `gw_debounce`, `gw_backoff`, `gw_devstate`.<br>• `gw_netsel`: interfaz activa entre Ethernet y Wi-Fi (NET-4).<br>• `gw_syslog`: líneas de log a RFC 5424 y anillo acotado (REL-4).<br>• `gw_tagdb`: imagen de proceso compartida.<br>• `gw_cfgdoc`: documento de configuración con los secretos aparte (exportar enmascarado, importar, migrar) y lector de JSON pegado en la consola (CFG-4, CFG-6, CFG-7).<br>• `gw_auth`: SHA-256, HMAC, PBKDF2 y Basic auth para la web (CFG-3).<br>• `gw_button`: gestos del botón BOOT (CFG-5, NET-5).<br>• `spb`: codec Sparkplug con nanopb.<br>• `gw_node`: motor del Edge Node con births, deaths, seq, alias, NCMD/DCMD y rebirth. | No (solo `port/gw_port_esp.c`) |
| `nanopb` | Runtime nanopb 0.4.9.1 vendorizado (Zlib) | No |
| `gw_rs485` | UART1 en `UART_MODE_RS485_HALF_DUPLEX` (RTS = GPIO21), expuesta como `mb_transport_t` | Sí |
| `gw_bus` | Tarea **dueña única del bus RS485**: encuesta y escrituras DCMD | Sí |
| `gw_edge` | Tarea Sparkplug: cliente esp-mqtt, bdSeq, will, backoff, motor `gw_node` | Sí |
| `gw_net` | W5500 por SPI (u openeth en QEMU), Wi-Fi STA opcional como alternativa o respaldo (NET-4), DHCP o IP estática por interfaz, IPv6 opcional con SLAAC y dirección estática (NET-7), ruta por defecto según `gw_netsel`, SNTP | Sí |
| `gw_bsp` | **Descripción declarativa de la placa** (`gw_bsp.c`): todos los pines, direcciones I²C y niveles activos (DI, DO, DE del RS485, I²C, LED y orden de color, buzzer, W5500). Los drivers la leen y no contienen datos de la placa | Sí (tipos) |
| `gw_board` | 8 DI con muestreo a 1 kHz y debounce; 8 DO en el TCA9554 (sobre `gw_dout`, sin pulsos al arrancar); LED WS2812 de estado | Sí |
| `gw_storage` | bdSeq en NVS; configuración desde LittleFS o embebida (solo si falta el archivo); escritura atómica de una configuración nueva; secretos en NVS cifrada (CFG-7); red de la última configuración válida para la recuperación; reset de fábrica y marca de puesta en marcha (CFG-5) | Sí |
| `gw_web` | Servidor HTTP: interfaz web embebida (CFG-3), API REST JSON de configuración y estado (CFG-4), Basic auth; mDNS y `_http._tcp` (NET-6) | Sí |
| `gw_ap` | Punto de acceso Wi-Fi de puesta en marcha (NET-5), aparte de `gw_net` | Sí |
| `gw_log` | Syslog remoto (REL-4): engancha `esp_log_set_vprintf`, copia cada línea a un anillo acotado y una tarea la envía por UDP | Sí |
| `gw_shell` | Shell `esp_console` en la consola: `status`, `devices`, `config [show\|load\|check]`, `restart`, `rebirth`, `mbread`, `ping` (y `di`, `boot` en QEMU). Respuestas atómicas respecto del log | Sí |
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
| `sparkplug` (`gw_edge`) | 0 / 8 | 6 KB estática | Sí; despierta como mínimo cada 1 s | Cliente MQTT, `gw_node_t` (seq, alias, estado RBE), las DO (llamadas a `gw_io_set_do`) |
| `esp_timer` (IDF) | — | IDF | — | Muestreo de DI (callback de 1 ms) |
| `status_led` | — / 2 | 2,5 KB | No (no crítica) | El RMT del LED |
| `console` (`gw_shell`) | — / 2 | 4 KB | No (no crítica) | La UART de la consola (REPL `esp_console`); lee `gw_tagdb` y `gw_status`; las escrituras y la lectura Modbus pasan por `gw_storage`, `gw_edge` y la cola del bus |
| `syslog` (`gw_log`, solo con `syslog.host`) | — / 2 | 3,5 KB estática | No (no crítica) | El socket UDP y el lado consumidor del anillo |
| `httpd` (`gw_web`) | — / 3 | 8 KB | No (no crítica) | El servidor HTTP; valida y guarda configuraciones (búferes en PSRAM si hay) |
| `boot_button` (`gw_board`) | — / 2 | 3 KB estática | No (no crítica) | GPIO0 (muestreo cada 20 ms), el buzzer |
| `mdns` (componente) | 0 / 1 | 4 KB | No | El responder mDNS |
| esp-mqtt, tcpip, eth, Wi-Fi (solo con `network.wifi`), event loop | IDF | IDF | IDLE vigiladas | El event loop es dueño de `gw_netsel` (eventos de enlace e IP y el temporizador de *failback*) |

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
- Al conectar se envían los SUBSCRIBE (NCMD, DCMD/+) y luego el NBIRTH y los DBIRTH.
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
- **Toda la memoria propia es estática** (`.bss` medido con `idf.py size-components`):
  - `main` unos 26 KB (`gw_config_t`);
  - `gw_edge` unos 27 KB: el motor `gw_node_t` con su búfer de payload de 6 KB, la pila de 6 KB y 4 búferes de comando de 1,2 KB;
  - `gw_bus` unos 10 KB: planificador, encuesta y pila de 4 KB;
  - `gw_core` unos 6 KB (`gw_tagdb`).
- Sin `malloc` en encuesta, codificación ni publicación.
- Se asigna memoria solo al arrancar: lectura de LittleFS y árbol de cJSON, ambos en PSRAM si la hay (§3.3).
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
| Buzzer | **Pasivo** (necesita PWM) | Demo: `ledcAttach(46, 1000 Hz, 8 bits)` con duty 200/255; ESPHome usa `rtttl` sobre `ledc` | Media (no se usa en el MVP) |
| LED | WS2812 en GPIO38, **orden de bytes RGB** (no GRB) | Demo: `neopixelWrite(pin, green, red, blue)` (intercambia R y G); ESPHome: `rgb_order: RGB` | Media |
| RS485 | UART1, TX 17, RX 18, RTS 21, modo `UART_MODE_RS485_HALF_DUPLEX` | Demo `WS_RS485.cpp`: `setPins(-1,-1,-1,21)` + `setMode(UART_MODE_RS485_HALF_DUPLEX)` | Alta |
| W5500 | SPI 13/14/15, CS 16, INT 12, RST 39 | Wiki y `WS_ETH.h` | Alta |
| Consola | USB-C → puente USB-UART CH34x en UART0 (GPIO 43/44) | La wiki enlaza el driver CH34XSER | Media. Si no hay logs, probar con `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` (el shell también funciona ahí) |

Todos estos datos están en un solo lugar, `components/gw_bsp/gw_bsp.c`. Las dos polaridades que solo se deducen de forma indirecta (DI y DO) se pueden invertir en `menuconfig` (menú `Gateway: board`); el resto es fijo. Arranque de las DO (`gw_dout`): el latch se carga con el nivel OFF de cada canal (0xFF con activa en bajo) **antes** de pasar los pines a salida; la escritura y la relectura se invierten según la máscara de polaridad.

**Pines de *strapping*** con periféricos de la placa: GPIO0 (BOOT), GPIO3 (RX del CAN, JTAG), GPIO45 (SD D0) y GPIO46 (buzzer). El MVP no los usa.

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

Resultado de `idf.py size` (build limpia con `idf.py set-target esp32s3 build`, 0 warnings):

| Región | Usado |
|---|---|
| Flash code (`.text`) | 886 076 B |
| Flash data (`.rodata`, etc.) | 226 208 B |
| DIRAM | 196 616 B de 341 760 (**57,5 %**): `.bss` 109 568, `.text` 62 719, `.data` 24 329 |
| IRAM | 16 384 B (100 %, lo normal en S3: el resto del código IRAM va en DIRAM) |
| Imagen total | **1 215 752 B** (29 % de la partición `ota_0` de 4 MB) |

**Costo de la configuración por web, shell, puesta en marcha y secretos (CFG-3 a CFG-7, NET-5, NET-6):** +122 856 B de imagen y +11 064 B de DIRAM (`.bss` +10 448: pila estática de 3 KB del botón, búfer de 1 KB de las respuestas del shell, búfer de estado de 1 KB y valores de tags de la web, mutex y semáforos). En ejecución, el servidor HTTP (pila de 8 KB y sockets), mDNS (pila de 4 KB) y los búferes de una configuración en validación (el documento, `gw_config_t` de 26 KB y el árbol cJSON) se asignan del heap, este último solo mientras se guarda y en PSRAM si hay.

**Costo del Wi-Fi (NET-4).** Antes: DIRAM 153 121 B (44,8 %) e imagen de 735 029 B. El driver Wi-Fi suma **27 327 B de DIRAM estática** (medido en su commit) (`.bss` +11 504, `.text` +8 352, `.data` +7 471) aunque la configuración no lo active, y 347 KB de flash. Ya se sacó de IRAM lo que IDF permite (`CONFIG_ESP_WIFI_IRAM_OPT=n`, `CONFIG_ESP_WIFI_RX_IRAM_OPT=n`). Al arrancarlo, sus búferes dinámicos y los de lwIP van a PSRAM cuando se puede (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`); el heap interno que consume en ejecución no se midió (no hay placa). Quien no lo necesite recupera todo con `CONFIG_GW_WIFI=n`.

**Costo del syslog remoto (REL-4):** 4,6 KB de `.bss` (pila estática de 3,5 KB de la tarea emisora y los búferes de una línea y un datagrama). El anillo (32 líneas de unos 240 B) se asigna al arrancar y solo si `syslog.host` está puesto, en PSRAM si la hay.

Tabla de particiones (`partitions.csv`, 16 MB): `nvs` 24 KB, `otadata`, `phy_init`, `ota_0`/`ota_1` 4 MB cada una (reservadas para FW-1), `storage` LittleFS 1 MB y `coredump` 64 KB.

### 3.3 Configuración

1. El firmware embebe `components/gw_storage/default_config.json`, que es el ejemplo de §6.
2. Para reemplazarla, copie el JSON a `idf/fs/config.json` y edítelo. `idf.py flash` graba la carpeta `fs/` en la partición `storage` y el firmware lee `/cfg/config.json` al arrancar.
3. Las claves opcionales fuera del MVP (`network.wifi`, NET-4; `network.ipv6`, NET-7; `syslog`, REL-4) están en `docs/SPEC.md` §6.1.
4. **La configuración embebida se usa solo si `/cfg/config.json` no existe** (y no hubo un reset de fábrica, §3.4). Cualquier otro problema deja el equipo en **recuperación**: la partición LittleFS no monta, el archivo está vacío, no se puede leer, supera el límite o es inválido. Entonces **no arranca la encuesta ni MQTT**; sí arrancan la red (con la de la última configuración válida, o DHCP por Ethernet), la interfaz web, el shell y el punto de acceso (NET-5), para corregirla sin regrabar. El LED parpadea en rojo y el log repite cada 10 s, con nivel ERROR, el motivo, por ejemplo:
   - `/cfg/config.json: 140000 bytes; el máximo es 131072 (CONFIG_GW_CONFIG_MAX_BYTES)`;
   - `partición LittleFS 'storage' no montable (ESP_FAIL)`;
   - `devices[0].tags[1].writable: solo las áreas coil y holding son escribibles`.

   Los errores semánticos llevan la **ruta JSON** del valor (`devices[1].tags[2].type: valor no válido (se esperaba bool|uint16|…)`, `devices[0].tags[0].scal: clave desconocida`). Los de sintaxis llevan **línea y columna**, contadas en caracteres a partir del puntero de error de cJSON (`JSON mal formado en la línea 5, columna 3: sintaxis no válida`); también se rechaza el contenido que sobra después del documento. Las **claves duplicadas** en cualquier objeto se rechazan (`mqtt.host: clave duplicada`): cJSON las acepta y sus búsquedas solo ven la primera.
5. **Límite de tamaño: 128 KiB** (`CONFIG_GW_CONFIG_MAX_BYTES`, menú `Gateway: storage`). El máximo del esquema (16 dispositivos, 256 tags con todos los campos) ocupa unos 46 KB en JSON compacto y 82 KB con sangría de 2 espacios. Al arrancar se asignan el búfer del archivo y el árbol de cJSON (unos 200 KB para 256 tags), **en PSRAM** si la hay; se liberan antes de arrancar las tareas. Sin PSRAM (QEMU) el límite real es el heap interno libre, y quedarse sin memoria se informa como tal.

6. **Cambiar la configuración sin regrabar:** la interfaz web (`http://<ip>/` o `http://<hostname>.local/`), la API REST (`PUT /api/config`, SPEC §6.1) o el shell (`config load`, pegar el JSON). En los tres casos se valida igual, se guarda de forma atómica (archivo temporal, `fsync`, `rename`) y el equipo se reinicia de forma ordenada (NDEATH). `config show` y `GET /api/config` exportan con los secretos enmascarados.

Opciones de compilación en `idf.py menuconfig`, en los menús `Gateway: ...`:

- polaridad de las DI y de las DO (los pines son fijos, en `gw_bsp`);
- prioridades y núcleos de las tareas;
- lecturas en bloque y hueco máximo;
- FC15/FC16 forzadas;
- backoff MQTT;
- Wi-Fi compilado o no (`CONFIG_GW_WIFI`, menú `Gateway: network`; sin él, `network.wifi` se ignora con un warning);
- espera de SNTP;
- líneas que guarda el syslog remoto (`CONFIG_GW_SYSLOG_SLOTS`, 32 por defecto) y prioridad de su tarea;
- tamaños de cola;
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

**Secretos (CFG-7):** `mqtt.password`, `network.wifi.password` y el hash de `web.password` (PBKDF2-HMAC-SHA256, 2048 iteraciones, sal aleatoria) están en NVS, nunca en `/cfg/config.json`. NVS se cifra con el esquema HMAC (`CONFIG_NVS_ENCRYPTION`, `CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC`, clave en el bloque eFuse **KEY5**), sin exigir cifrado de flash. El shell lo informa (`secrets  NVS encrypted (HMAC key in eFuse KEY5)`).

- **Proceso de fábrica.** El primer arranque genera una clave HMAC aleatoria y la **quema en KEY5 con propósito `HMAC_UP`: es irreversible** y la clave no se puede leer después (las claves de NVS se derivan con el periférico HMAC). Ese bloque queda ocupado: secure boot (FW-2) y cifrado de flash (FW-3) deben usar otros. Si se prefiere quemarla en la línea de producción: `espefuse.py burn_key BLOCK_KEY5 clave.bin HMAC_UP` antes del primer arranque. Una placa con KEY5 ya usado para otra cosa no arranca NVS (el log lo dice y bdSeq queda en RAM).
- **Respaldo para desarrollo.** Con `CONFIG_NVS_ENCRYPTION=n` (por ejemplo, para no quemar eFuses en una placa de pruebas) los secretos siguen fuera del JSON, pero en NVS sin cifrar; el shell lo avisa (`NOT encrypted`). QEMU emula el periférico HMAC y el eFuse (el archivo `efuse.bin` de la corrida), así que el E2E corre con NVS cifrada.
- **Migración.** Un `config.json` con secretos en claro (grabado con `idf.py flash` o de un firmware anterior) se acepta: al arrancar, los secretos pasan a NVS, se **borra la partición LittleFS entera** (LittleFS es *copy-on-write* y los bloques viejos guardarían el texto) y se reescribe el archivo sin ellos. Durante esa ventana el equipo queda marcado para puesta en marcha: un corte de energía no lo deja con la identidad de ejemplo. Al activar el cifrado en un equipo que ya tenía NVS sin cifrar, NVS se borra (bdSeq y secretos): hay que volver a cargar los secretos (PUT con los valores).

## 4. Verificación (evidencia real)

### 4.1 QA-1: tests de host (CMake + Unity v2.6.1 + CTest)

```bash
cmake -S idf/test/host -B build-host && cmake --build build-host -j3 && ctest --test-dir build-host
```

Resultado: **15/15 ejecutables y 100/100 casos OK**, compilados con `-Wall -Wextra -Werror -Wconversion`.

| Suite | Casos | Qué cubre |
|---|---|---|
| `test_mb_rtu` | 6 | CRC (vectores de la especificación Modbus), tramas FC01–06/15/16, longitud de respuesta, excepciones, eco erróneo |
| `test_mb_master` | 8 | t3,5 y retardo entre tramas, timeout y reintentos, CRC malo, unidad equivocada, excepción sin reintento |
| `test_conv` | 5 | Mapeo de datatypes §6, int16/uint16/int32/uint32/float32, ABCD/CDAB, escala inversa (12,3 → 123), rangos |
| `test_logic` | 6 | RBE y deadband contra el último valor reportado (deriva), validez y NaN, debounce, backoff 1–60 s, fuera de línea |
| `test_config` | 18 | Sección `web` (usuario, contraseña, puerto), `network.hostname` y `mdns` (validación y nombre derivado), campos del error (ruta, línea, columna) y anidamiento máximo; ejemplo §6 literal, sección `syslog`, sección `network.wifi` (valores por defecto, SSID, contraseña WPA2/PSK/abierta, rol, `failback_s`, IP estática), literales IPv6 (`::`, grupos de 4 dígitos, IPv4 embebida, 16 formas inválidas) y sección `network.ipv6`, JSON embebido, valores por defecto, reglas del esquema, errores con ruta (`devices[1].tags[2].type`), sintaxis con línea y columna (también con UTF-8 y contenido sobrante), claves duplicadas, límites 16/64/256, documento máximo (256 tags, > 40 KB), asignador del árbol y falta de memoria |
| `test_plan` | 2 | Lecturas en bloque, solapes, huecos, límite de 125 registros |
| `test_spb` | 7 | Ida y vuelta de todos los tipos, formato wire (Int16 con extensión de signo), NDEATH sin seq, peor caso de DBIRTH en el búfer |
| `test_node` | 8 | NBIRTH y DBIRTH (nombre, alias, datatype, valor; alias únicos), DDEATH, DDATA por excepción, seq compartido con vuelta a 0, rebirth, NCMD DO/Reboot, DCMD por nombre y alias, descartes |
| `test_poll` | 9 | Encuesta contra un esclavo simulado, fuera de línea tras N ciclos, excepción, un bloque sin respuesta (ciclo fallido, tags sin refrescar, DDEATH, sin vuelta a línea con el ciclo incompleto), contadores por dispositivo, planificación, escritura FC06 con relectura, FC15/FC16 forzadas |
| `test_netsel` | 5 | Selección de interfaz (NET-4): solo Ethernet (sin conmutaciones), Wi-Fi de respaldo (*failover* inmediato, *failback* tras `failback_s`), Wi-Fi primaria, enlace preferido que rebota (no roba la sesión), `failback_s` = 0 |
| `test_syslog` | 5 | REL-4: severidad del formato de `esp_log`, parseo de líneas (colores, hora del sistema, `: ` en el mensaje, truncado), timestamps RFC 3339 (bisiesto, 2099), formato RFC 5424 (PRI, NILVALUE, saneado de HOSTNAME/APP-NAME, caracteres de control, truncado), anillo lleno sin esperar y conteo de descartes |
| `test_auth` | 5 | SHA-256 (FIPS 180-4, bordes de relleno, un millón de `a` en trozos), HMAC (RFC 4231), PBKDF2 (RFC 7914 y vectores publicados), hash de contraseña con sal (igual que `hashlib`), Basic auth |
| `test_cfgdoc` | 6 | Secretos: PUT nuevo (documento sin secretos), máscara que conserva, ausente o `""` que borra, archivo con secretos en claro (migración), errores con ruta, exportación enmascarada y reimportación, lector de JSON de la consola |
| `test_button` | 4 | BOOT: reset a los 10 s (una vez), gesto de 2–10 s, botón presionado desde el arranque, rebotes, vuelta del reloj |
| `test_dout` | 6 | Polaridad de 8 canales (activa en bajo, en alto y mixta), arranque del TCA9554 (latch OFF antes de CONFIG, también en arranque en caliente), DO/3 → pin bajo y relectura, errores de bus |

Los tests de host descargan Unity y cJSON con `FetchContent`, desde GitHub y con tag fijo.

### 4.2 Interoperabilidad Sparkplug

```bash
mkdir -p /tmp/spb && build-host/spb_dump /tmp/spb && python3 idf/tools/spb_decode.py --check /tmp/spb
```

`spb_dump` genera NDEATH, NBIRTH, DBIRTH, DDATA y NDATA reales del motor. `spb_decode.py` los compila con `grpc_tools.protoc` a partir del `sparkplug_b.proto` oficial de eclipse/tahu (copiado sin cambios en `components/gw_core/proto/`) y los decodifica con **Google protobuf 7.36**.

Resultado: **5 payloads decodificados, 0 errores estructurales**. Además se decodificaron los 63 mensajes capturados en el E2E.

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
- UART1 → socket → **pymodbus 3.8.6** como esclavo RTU, unidades 1 (`Meter1`, tags de §6) y 2 (`Bulk`, 60 registros).
- UART0 → socket, para el log y el shell (las DI se inyectan con su comando `di`).
- **mosquitto 2.0.18** privado (copia renombrada del binario), un respondedor SNTP propio en el UDP 123 y un receptor syslog UDP (`--syslog-port`, por defecto 15514). El harness solo envía señales a los procesos que arrancó él.
- **Aislamiento de red.** El harness se mueve a un *network namespace* propio, con solo la interfaz de loopback (necesita `CAP_SYS_ADMIN`, por ejemplo root en un contenedor). Todos los puertos, también el UDP 123, son privados de la corrida, así que dos corridas en paralelo no chocan. Sin ese permiso (o con `--no-isolate`) usa el loopback del host con los puertos de `--mqtt-port`, `--modbus-port` y `--console-port` (o `E2E_MQTT_PORT`, `E2E_MODBUS_PORT`, `E2E_CONSOLE_PORT`); por defecto 18831, 15020 y 15555. Los argumentos extra de `run_e2e.sh` pasan a `e2e.py`.
- El harness escribe en la imagen bajo prueba su propio `config.json`, con el puerto MQTT elegido. Para los arranques de configuración genera variantes de la imagen con otra partición `storage` (con `littlefs-python`, el mismo que usa la build) o con la partición borrada. El límite de tamaño se lee de `build-qemu/sdkconfig` (`CONFIG_GW_CONFIG_MAX_BYTES`).
- "No arrancó nada" se comprueba con una señal positiva: la línea `nothing is started` y, 6 s después, la respuesta de `status` del shell (`nothing is running: no polling, no MQTT; recovery network and web UI only`). La ausencia de logs de arranque de la UART RS485 y de MQTT, de NBIRTH y de peticiones al esclavo Modbus queda como comprobación adicional.
- HTTP: slirp reenvía `127.0.0.1:18080` del host al puerto 80 del equipo (`--http-port`). El shell se consulta con un helper común que repite la pregunta hasta ver todas las líneas esperadas (#56). `--only failsafe,network,web,reset,main` corre solo algunos escenarios.

Configuración usada: `tools/e2e/fs/config.json` (keepalive de 10 s, syslog hacia el receptor del harness).

Resultado: **67/67 comprobaciones OK**.

| Criterio | Resultado medido |
|---|---|
| CFG-1 / CFG-2: arranques con `config.json` de 131 073 B, vacío, con error de sintaxis, con error de esquema y con la partición borrada; y sin archivo | OK: en los cinco primeros no arranca la encuesta ni MQTT (ni UART RS485, 0 NBIRTH, 0 peticiones Modbus; solo la red de recuperación y la web), el LED pasa a error y el log ERROR da el motivo (`… 131073 bytes; el máximo es 131072`, `línea 5, columna 3`, `devices[0].tags[0].type`, `no montable`). Sin archivo: configuración embebida |
| §7.1 NBIRTH con seq = 0 y bdSeq; al matar la conexión TCP (SIGKILL a QEMU) el broker publica el NDEATH con el mismo bdSeq | OK: NBIRTH a 1,9 s del arranque; NDEATH con el bdSeq de la sesión |
| Reinicio del broker (SIGTERM, 9 s parado) | OK: mosquitto publica el will (NDEATH, bdSeq de la sesión) al cerrarse; backoff 1000 → 1851 → 3745 → 7181 ms con bdSeq nuevo en cada intento; NBIRTH (seq 0, bdSeq nuevo) y DBIRTH de los dos equipos (seq 1 y 2) |
| Broker congelado (SIGSTOP 20 s > 1,5 × keepalive) | OK: el gateway declara muerta la sesión por keepalive a los 10,0 s (`No PING_RESP`); tras SIGCONT llegan los will de la sesión muerta y del intento hecho durante el congelamiento, y luego NBIRTH con bdSeq nuevo y los dos DBIRTH; la sesión nueva sigue viva |
| §7.2 Esclavo apagado → DDEATH; encendido → DBIRTH | OK: DDEATH en 3,05 s (cota 5,8 s); DBIRTH 0,67 s después de volver |
| §7.3 DI → NDATA en menos de 100 ms; NCMD DO/3 = true → NDATA | OK: 23 ms (incluye 20 ms de debounce); NDATA alias 22 = true (pines simulados con la polaridad de `gw_bsp`) |
| §7.4 DCMD Setpoint = 12,3 → holding 10 = 123 por FC06 y DDATA | OK: el esclavo registró `(6, 10, [123])`; DDATA 12,3 |
| §7.5 NCMD Rebirth → NBIRTH (seq 0, mismo bdSeq) y todos los DBIRTH | OK |
| §7.6 Todos los payloads se decodifican con el proto oficial | OK: 63 mensajes, 0 errores |
| CFG-6 shell en la consola; REL-5 | OK: `devices` da `Meter1 unit 1 online cycles 2 failed 0 \| ok 6 …` y, con el esclavo apagado, `offline … failed 3 \| … timeout 9`; `config` da el resumen; `status` responde con la configuración válida (`config … ok`, `MQTT connected`, `Modbus 2/2 devices online`) y también con la configuración rechazada. NBIRTH `Node Info/Reset Reason` = `power-on` en el primer arranque y `software` tras NCMD Reboot |
| NET-4 sin radio y NET-7: configuración con `network.wifi` y `network.ipv6` (nodo `GW02`, arranque aparte) en la build de QEMU | OK: arranca, avisa `network.wifi ignored: Wi-Fi is not available in this build` y publica NBIRTH por Ethernet; el shell muestra `iface ethernet up 10.0.2.15 (active) \| wifi not configured` y el resumen de `network.wifi`. Con la configuración normal, `status` muestra Ethernet como única interfaz activa. IPv6: `fe80::5054:ff:fe12:3456` (link-local), `fec0::5054:ff:fe12:3456` (SLAAC con el RA de slirp) y `fec0::1234` (estática); `ping fec0::2`: 3 enviados, 3 respondidos. MQTT sobre IPv6 no se puede probar aquí (§6.6) |
| REL-4 syslog remoto (UDP al receptor del harness) | OK: `<134>1 2026-11-04T01:03:09.035Z GW01 edge - - [meta sequenceId="21"] Sparkplug session up (bdSeq=0)`: *facility* local0, HOSTNAME = nodo, APP-NAME = módulo, hora SNTP; 22 datagramas al primer NBIRTH, 0 mal formados; `status` da `syslog 10.0.2.2:15514 connected, sent 22, dropped 0, errors 0`. Con el broker caído llega `<132> … MQTT down; reconnect attempt 1 in 1000 ms`. 119 datagramas en 3 arranques, `sequenceId` sin huecos en cada uno |
| CFG-3 / CFG-4 recuperación (cinco archivos rechazados) | OK: la red arranca (DHCP), la web y `/api/status` responden con la contraseña de puesta en marcha (leída de la consola) y el motivo; 0 peticiones Modbus, 0 NBIRTH. Con el error de esquema, el documento se exporta, se corrige y se guarda por la API: el equipo reinicia y publica NBIRTH (`GW03`) |
| CFG-4 API REST (nodo `GW04`) | OK: sin credenciales o con otra contraseña → 401; `GET` con `mqtt.password` y `web.password` = `********`; `PUT` inválido → 400 `devices[0].tags[0].type`; sintaxis → 400 con línea 5, columna 9; `dry_run` → 200 sin guardar; 131 074 B → 413; `PUT` válido → NDEATH de `GW04`, NBIRTH de `GW05`, la misma contraseña sigue valiendo |
| CFG-3 interfaz web | OK: `/` (≈14 KB, HTML) y `/api/status` (modo, MQTT, `Meter1` en línea, `Voltage` = 230,5, Ethernet activa) |
| CFG-6 shell de configuración | OK: `config show` con 2 secretos enmascarados y ninguno en claro; `mbread 1 holding 10` = 50, `mbread 1 input 0 2` = 17254 32768 (230,5), unidad 9 → error; `rebirth` → NBIRTH seq 0 y DBIRTH seq 1 y 2; `config load` inválido → ruta; `config check` válido sin guardar; `config load` válido → NDEATH, NBIRTH de `GW06` |
| CFG-7 secretos | OK: al primer arranque `secrets moved from /cfg/config.json to NVS`; `NVS partition "nvs" is encrypted` (QEMU emula HMAC y eFuse); el shell dice `NVS encrypted (HMAC key in eFuse KEY5)`; ni la contraseña MQTT ni la web aparecen en claro en ningún lugar de la imagen de flash |
| CFG-5 reset de fábrica y NET-5 | OK: `boot 1` 3 s → gesto de punto de acceso (sin reset; la build de QEMU avisa que no hay radio); `boot 1` sostenido → aviso a los 2,1 s, reset a los 10,1 s, NDEATH ordenado, arranque en puesta en marcha (LED azul, 0 NBIRTH, ni la identidad de ejemplo); la contraseña web vieja → 401; la de puesta en marcha nueva abre `/api/status` (`commissioning`); una configuración cargada por la API arranca el equipo (`GW08`); en la partición no queda ningún bloque de la configuración vieja |
| NET-6 | OK: `mdns: gw01.local, _http._tcp port 80 ('Plant1/GW01')` (nombre derivado de `edge_node_id`). Las respuestas no se pueden probar con `open_eth` (§6.2.1) |
| Extras | OK: DDATA solo por excepción, solo alias, deadband 0,1 respetada; DCMD por alias (FC05) y DCMD sobre métrica de solo lectura descartado con log; bdSeq persistido en NVS tras un corte de energía; NCMD Reboot publica NDEATH QoS 1 y **un solo** NDEATH (will descartado); DBIRTH de 2684 B (mayor que el búfer de 2 KB de esp-mqtt); 60 registros leídos con una sola FC03; timestamps UTC vía SNTP; continuidad de seq |

**Lo que QEMU no cubre**, y por qué la build de emulador difiere (`sdkconfig.qemu`):

- **W5500.** No está emulado; se usa `open_eth`.
- **TCA9554, GPIO y RMT.** No están emulados; se usa `CONFIG_GW_IO_SIMULATED`: DO en RAM, DI con el comando `di` del shell y LED solo en el log.
- **Modo RS485 de la UART.** `uart_wait_tx_done` hace `assert` por `TX_DONE`; se usa UART normal.
- **PSRAM octal.** QEMU da *segfault*; se deshabilita.
- **Flash QIO.** Las lecturas de la app llegan desplazadas 2 bytes; se usa DIO.
- **IPv6 hacia el host.** slirp entrega las conexiones a `fec0::2` al `::1` del host y el kernel del contenedor no tiene IPv6: solo se prueban direcciones e ICMPv6.
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
| MQTT-2 | Implementado | `gw_edge.c`, `gw_core/src/gw_backoff.c` | H + Q (reinicio del broker y broker congelado: NDEATH, bdSeq nuevo, NBIRTH y todos los DBIRTH) |
| MQTT-3 | Implementado | `gw_edge.c` (`prepare_attempt`) | Q |
| MQTT-7 | Implementado | `gw_edge.c`, `gw_bus.c` | Colas y búferes estáticos, `outbox.limit` y `SKIP_PUBLISH_IF_DISCONNECTED`. Sin prueba de estrés |
| SPB-1 | Implementado | `gw_core/src/spb.c`, `proto/` | H + interoperabilidad Python + Q |
| SPB-2 | Implementado | `gw_node.c`, `gw_edge.c` (`op_reboot`) | H + Q |
| SPB-3 | Implementado | `gw_storage/gw_storage.c`, `gw_edge.c` | Q (por intento y persistido) |
| SPB-4 | Implementado | `gw_core/src/gw_node.c` (`publish_nbirth`) | H + Q. Métricas de nodo: bdSeq, Rebirth, Reboot, `Node Info/Firmware`, `Hardware`, `Reset Reason` (alias 1–6), DI/1–8 y DO/1–8 |
| SPB-5 | Implementado | `gw_node.c` | H (vuelta 255 → 0) + Q (continuidad) |
| SPB-6 | Implementado | `gw_node.c` (`gw_node_device_changed`) | H + Q |
| SPB-7 | Implementado | `gw_node.c`, `gw_rbe.c` | H + Q |
| SPB-8 | Implementado | `gw_node.h` (mapa de alias) | H + Q |
| SPB-9 | Implementado | `gw_node.c` (`gw_node_rebirth`) | H + Q |
| SPB-10 | Implementado | `gw_node.c`, `gw_poll.c` (`gw_poller_write`) | H + Q |
| SPB-11 | Implementado | `gw_node.c`, `gw_board/gw_io.c` | H + Q (DO simulada con la polaridad de la placa; TCA9554 C) |
| SPB-12 | Implementado | `gw_rbe.c` | H + Q |
| MB-1 | Implementado | `components/gw_rs485/gw_rs485.c` | Q a 9600 8N1; otras paridades C |
| MB-2 | Implementado | `gw_rs485.c` (`UART_MODE_RS485_HALF_DUPLEX`) | **C**: QEMU no soporta el modo; la temporización de RTS/DE no está verificada |
| MB-3 | Implementado | `gw_core/src/mb_rtu.c`, `gw_poll.c` | H (FC01–04) + Q (FC01, FC03, FC04) |
| MB-4 | Implementado | `mb_rtu.c`, `gw_poll.c` | H (las cuatro) + Q (FC05, FC06). FC15/FC16 de 16 bits con `CONFIG_GW_MB_FORCE_MULTI_WRITE` |
| MB-5 | Implementado | `gw_core/src/gw_conv.c` | H + Q (float32, int16, uint16, bool) |
| MB-7 | Implementado | `gw_conv.c` | H + Q |
| MB-8 | Implementado | `gw_core/src/mb_master.c` | H + Q |
| MB-9 | Implementado | `gw_devstate.c`, `gw_poll.c` | H (también un bloque sin respuesta mientras otro responde) + Q |
| MB-10 | Implementado | `gw_poll.c` (`gw_poller_next`) | H + Q |
| MB-16 | Implementado | `components/gw_bus/gw_bus.c` | Diseño: una tarea y una cola |
| IO-1 | Implementado | `components/gw_board/gw_io.c`, `gw_debounce.c`, `gw_bsp` | H (debounce y polaridad) + Q (pines simulados con la polaridad de `gw_bsp`); GPIO reales C |
| IO-2 | Implementado | `gw_core/src/gw_dout.c`, `gw_io.c`, `gw_bsp` | H (secuencia de arranque, activa en bajo, mezclas, errores de bus) + Q (registros simulados); I²C real C |
| IO-5 | Implementado | `components/gw_board/gw_status.c` | Q (estados en el log); RMT/WS2812 C; colores sin verificar |
| CFG-1 | Implementado | `gw_core/src/gw_config.c`, `gw_storage/gw_storage.c`, `main/main.c` | H (documento máximo de 256 tags, falta de memoria, rutas JSON, línea y columna, claves duplicadas) + Q (archivo demasiado grande, vacío, con error de sintaxis o de esquema y partición sin montar: nada arranca, LED de error, log ERROR con el motivo) |
| CFG-2 | Implementado | `gw_storage/gw_storage.c`, `main/CMakeLists.txt` | Q (LittleFS; respaldo embebido solo si falta el archivo) |
| FW-4 | Implementado | `idf/CMakeLists.txt` (`PROJECT_VER`) | Q (`0.1.0+g<hash>[.dirty]` en NBIRTH) |
| REL-1 | Implementado | `gw_bus.c`, `gw_edge.c`, `sdkconfig.defaults` | C/Q (configurado; sin inyección de fallos) |
| REL-2 | **Parcial** | Todo `gw_*` | El código propio es estático y no asigna en el camino crítico; **lwIP y esp-mqtt asignan del heap internamente** (acotado, no estático) |
| REL-3 | Implementado | `esp_log` en todos los módulos | Q. Con REL-4 la consola sigue igual |
| QA-1 | Implementado | `test/host/` | 100 casos |
| QA-2 | Implementado | — | 0 warnings; tamaño en §3.2 |

**Fuera del MVP**, implementado total o parcialmente:

- **MB-11** Parcial: bloques con hueco máximo global por Kconfig, no por dispositivo.
- **MB-13** Parcial: excepciones decodificadas y en el log; contadores por dispositivo (ciclos, ciclos fallidos e intentos ok / timeout / CRC / trama / excepción) en el shell y en el log de fuera de línea, no publicados por Sparkplug.
- **CFG-3** Implementado: interfaz web embebida (una página, sin recursos externos) con estado en vivo (`/api/status`: modo, interfaces, MQTT, dispositivos con contadores y valores, DI/DO, heap), formularios de red, MQTT, Sparkplug, RS485, credenciales y dispositivos/tags, editor JSON, validar, guardar, importar, exportar y reiniciar; Basic auth con hash PBKDF2. Arranca también con la configuración rechazada (recuperación) y en puesta en marcha. Evidencia: H (`gw_auth`: vectores SHA-256, HMAC RFC 4231, PBKDF2 RFC 7914, Basic) + Q (página, estado, 401, recuperación en los cinco casos de archivo rechazado, corrección por la API que arranca el equipo). Sin TLS (§6.2.1).
- **CFG-4** Implementado: API REST JSON (`GET/PUT/POST /api/config`, `?dry_run=1`, `?download=1`, `POST /api/restart`): validación con la ruta JSON o línea y columna del error, escritura atómica, reinicio ordenado (NDEATH), secretos enmascarados al exportar. H (`gw_cfgdoc`, campos de error, profundidad) + Q (401, export enmascarado, 400 con ruta, 400 con línea, dry run, 413, PUT válido → NDEATH y NBIRTH con la configuración nueva, el secreto se conserva con la máscara).
- **CFG-5** Implementado: reset de fábrica con BOOT 10 s (gestos en `gw_button`, host-tested), borrado de LittleFS completo y de NVS, modo puesta en marcha sin la identidad de ejemplo. H + Q (botón simulado: gesto de 3 s sin reset, reset a los 10 s, NDEATH, puesta en marcha, contraseña vieja rechazada, configuración cargada por la API, ningún bloque viejo en la partición). GPIO0 real, LED y buzzer: C.
- **CFG-6** Implementado: shell con `status`, `devices`, `config` (resumen), `config show` (secretos enmascarados), `config load` / `config check` (pegar un JSON: validar, guardar de forma atómica y reiniciar), `restart`, `rebirth`, `mbread` (lectura Modbus por el dueño del bus) y `ping`; respuestas atómicas respecto del log (#56). Funciona con la configuración rechazada. Q (show, mbread de registro, float32 y equipo ausente, rebirth, load rechazado con ruta, check, load válido que reinicia). Sin contraseña (acceso físico); `CONFIG_GW_SHELL_WRITE_COMMANDS=n` lo deja de solo lectura.
- **CFG-7** Implementado: secretos en NVS cifrada con la clave HMAC en eFuse KEY5, fuera del JSON; migración de un archivo con secretos en claro (con borrado de la partición). Q (QEMU emula HMAC y eFuse: NVS cifrada; ningún secreto en claro en toda la imagen de flash tras la migración). El quemado del eFuse en placa: C (irreversible, §3.4).
- **NET-5** Implementado, **sin probar con radio**: punto de acceso `GW-Setup-XXXXXX` (WPA2, contraseña de puesta en marcha) sin configuración válida y con BOOT 2–10 s. Q (los dos disparadores llegan a `gw_ap`; la build de QEMU no tiene radio) + C (SoftAP, AP+STA).
- **NET-6** Implementado: `network.hostname` (o derivado de `edge_node_id`) en DHCP y mDNS con `_http._tcp`. H (esquema, derivación) + Q (el responder arranca con el nombre derivado) + C (respuestas mDNS: `open_eth` no tiene filtro multicast, §6.2.1).
- **MB-14** Parcial: escrituras antes del siguiente sondeo, no entre bloques.
- **IO-3** Parcial: DO a OFF al reiniciar, sin el resto del modo seguro.
- **IO-6** Parcial: solo dos pitidos al confirmar el reset de fábrica (C).
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
- el modo RS485 por hardware (temporización de RTS/DE a 115200 incluida);
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
  - rangos elegidos aquí: `response_timeout_ms` 10–5000, `retries` 0–5, `poll_ms` ≥ 50, `keepalive_s` 5–3600, `inter_frame_delay_ms` ≤ 1000.

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
- Buzzer (IO-6): solo los pitidos del reset de fábrica, con LEDC a 2,7 kHz; sin verificar el tipo de buzzer.
- **QEMU y los reinicios.** En el emulador `esp_restart` no reinicia la UART1 ni la matriz de interrupciones: si llega una respuesta Modbus durante el reinicio, el arranque siguiente se cuelga en `uart_driver_install` (tormenta de interrupciones). Los reinicios ordenados (REST, shell, NCMD Reboot, reset de fábrica) silencian el bus antes (`gw_bus_quiesce`), lo que además evita cortar una trama en la placa.

### 6.3 Comportamientos Sparkplug elegidos

- Una lectura fallida (excepción) se publica como `is_null` en DDATA. No hay calidad (SPB-16).
- Un dispositivo arranca fuera de línea y solo se publica su DBIRTH tras la primera respuesta.
- **Un ciclo falla si alguna petición queda sin respuesta tras los reintentos** (una excepción Modbus cuenta como respuesta). El ciclo se corta ahí, sin gastar el timeout en los bloques restantes, y los ciclos fallidos cuentan para `offline_after`. Así el DDEATH llega en `offline_after` ciclos aunque los demás bloques del equipo respondan.
- Los tags de los bloques no leídos **conservan el valor y el timestamp de su última lectura**: no se reportan como frescos. Si la falta persiste, el DDEATH los invalida a todos. Un equipo pasa (o vuelve) a estar en línea solo tras un ciclo completo, así que su DBIRTH nunca lleva valores que no se leyeron.
- Un DCMD a un dispositivo fuera de línea se descarta, no se encola.
- Las escrituras se reintentan como cualquier transacción. FC05/06/15/16 son idempotentes, pero un esclavo que ejecuta y no responde recibe la escritura dos veces.
- **Rebirth espontáneo:** si el primer NBIRTH salió antes de que sincronizara SNTP (timestamps de 1970), se reemite al sincronizar. Esta rama no se ejercitó en el E2E.

### 6.4 Reloj

Sin SNTP no hay hora válida: no hay RTC (TIME-2). Antes de la primera conexión se esperan hasta 3 s a SNTP (`CONFIG_GW_SNTP_WAIT_MS`). Se desactivó el retardo aleatorio de arranque de SNTP de lwIP, porque en una de las corridas empujó la sincronización más allá del NBIRTH.

### 6.5 Broker lento y límites de comandos

- Un broker lento bloquea la tarea Sparkplug hasta 5 s por publicación (`network.timeout_ms`). La memoria no crece: los cambios se **coalescen** y se publica el último valor; los intermedios se pierden. No es store & forward.
- Los comandos de más de 1024 bytes, o fragmentados por esp-mqtt, se descartan con log.
- Con 8 escrituras pendientes, las nuevas se descartan con log.

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

- Una DI que cambia más rápido que el debounce se pierde.
- El timestamp de NDATA es el del lote de cambios, no el de cada bit.
- Las DO pasan a OFF en cada arranque (en frío, en caliente, por watchdog o por panic) **antes de cargar la configuración**, así que también quedan en OFF si la configuración se rechaza. El TCA9554 no se reinicia con el ESP32 y conserva el latch; por eso el firmware lo reescribe con el nivel OFF antes de pasar los pines a salida.
- La polaridad de las DO (activa en bajo) se dedujo del demo y de ESPHome, sin esquemático ni placa. Si la medición dice lo contrario, basta con `CONFIG_GW_DO_ACTIVE_LOW=n`.

### 6.10 Pruebas y entorno

- La versión de firmware lleva `.dirty` hasta que el coordinador haga el commit.
- `test_poll` (`test_unanswered_block_fails_the_cycle`) falló una vez en 7 corridas con `Expected …756 Was …757`: el timestamp de un tag no leído cambió 1 ms. Parece depender del reloj de pared del host (un cambio de milisegundo durante el ciclo); no es de esta rama y no se tocó.
- Los tests de host necesitan red la primera vez (FetchContent de Unity y cJSON).
- Mientras corría el E2E, otro proceso del host compartido ocupó el UDP 123 y terminó el mosquitto por nombre. Por eso el harness usa una copia renombrada del broker y corre en su propio *network namespace* (§4.3). Sin aislamiento, si el UDP 123 está ocupado el check TIME-1 falla: exige que haya respondido el SNTP del harness.
- **pymodbus se fijó en 3.8.6**: la 3.16 cambió el API del datastore (SimDevice).
- `idf.py` no acepta `-j`: la build usó el paralelismo por defecto de ninja.
- Hay dos componentes de terceros: nanopb (vendorizado) y `joltwallet/littlefs`. El paquete `livekit/nanopb` del registro se descartó por un defecto de ABI. `espressif/esp_io_expander_tca9554` se descartó: carga el latch con 0xFF, que con las DO activas en bajo es justamente OFF, pero no permite probar en host la secuencia ni la polaridad (`gw_dout`). La viabilidad (`docs/feasibility-idf.md`, IO-2) lo descartaba suponiendo, por error, DO activas en alto.
