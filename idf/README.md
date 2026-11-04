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
| `gw_core` | **Lógica pura en C, sin IDF; se prueba en el host:**<br>• `gw_config`: parseo y validación JSON §6.<br>• `gw_dout`: secuencia de arranque del TCA9554 y polaridad de las E/S.<br>• `mb_rtu`: CRC y tramas FC01–06/15/16.<br>• `mb_master`: transacciones, t3,5, timeout y reintentos.<br>• `gw_conv`: tipos, escala, orden de palabras y datatype Sparkplug.<br>• `gw_plan`: lecturas en bloque.<br>• `gw_poll`: encuesta, fuera de línea y escrituras.<br>• `gw_rbe`: reporte por excepción y deadband.<br>• `gw_debounce`, `gw_backoff`, `gw_devstate`.<br>• `gw_tagdb`: imagen de proceso compartida.<br>• `spb`: codec Sparkplug con nanopb.<br>• `gw_node`: motor del Edge Node con births, deaths, seq, alias, NCMD/DCMD y rebirth. | No (solo `port/gw_port_esp.c`) |
| `nanopb` | Runtime nanopb 0.4.9.1 vendorizado (Zlib) | No |
| `gw_rs485` | UART1 en `UART_MODE_RS485_HALF_DUPLEX` (RTS = GPIO21), expuesta como `mb_transport_t` | Sí |
| `gw_bus` | Tarea **dueña única del bus RS485**: encuesta y escrituras DCMD | Sí |
| `gw_edge` | Tarea Sparkplug: cliente esp-mqtt, bdSeq, will, backoff, motor `gw_node` | Sí |
| `gw_net` | W5500 por SPI (u openeth en QEMU), DHCP o IP estática, SNTP | Sí |
| `gw_bsp` | **Descripción declarativa de la placa** (`gw_bsp.c`): todos los pines, direcciones I²C y niveles activos (DI, DO, DE del RS485, I²C, LED y orden de color, buzzer, W5500). Los drivers la leen y no contienen datos de la placa | Sí (tipos) |
| `gw_board` | 8 DI con muestreo a 1 kHz y debounce; 8 DO en el TCA9554 (sobre `gw_dout`, sin pulsos al arrancar); LED WS2812 de estado | Sí |
| `gw_storage` | bdSeq en NVS; configuración desde LittleFS o embebida (solo si falta el archivo) | Sí |
| `gw_shell` | Shell de diagnóstico `esp_console` en la consola: `status`, `devices`, `config` (y `di` en QEMU) | Sí |
| `main` | `app_main`: carga la configuración y conecta los componentes | Sí |

Dependencias gestionadas, **con versión fija** (`==`) en cada `idf_component.yml` y hash en `dependencies.lock`:

- `espressif/mqtt` 1.1.0
- `espressif/w5500` 2.0.1
- `espressif/cjson` 1.7.19~2
- `espressif/led_strip` 3.1.1
- `joltwallet/littlefs` 1.23.1

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
| `console` (`gw_shell`) | — / 2 | 4 KB | No (no crítica) | La UART de la consola (REPL `esp_console`); solo lee `gw_tagdb` y `gw_status` |
| esp-mqtt, tcpip, eth, event loop | IDF | IDF | IDLE vigiladas | — |

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

Resultado de `idf.py size` (build limpia con `idf.py set-target esp32s3 build`, 1165/1165 objetivos, 0 warnings):

| Región | Usado |
|---|---|
| Flash code (`.text`) | 491 784 B |
| Flash data (`.rodata`, etc.) | 156 400 B |
| DIRAM | 153 121 B de 341 760 (**44,8 %**): `.bss` 82 768, `.text` 53 895, `.data` 16 458 |
| IRAM | 16 384 B (100 %, lo normal en S3: el resto del código IRAM va en DIRAM) |
| Imagen total | **734 957 B** (18 % de la partición `ota_0` de 4 MB) |

Tabla de particiones (`partitions.csv`, 16 MB): `nvs` 24 KB, `otadata`, `phy_init`, `ota_0`/`ota_1` 4 MB cada una (reservadas para FW-1), `storage` LittleFS 1 MB y `coredump` 64 KB.

### 3.3 Configuración

1. El firmware embebe `components/gw_storage/default_config.json`, que es el ejemplo de §6.
2. Para reemplazarla, copie el JSON a `idf/fs/config.json` y edítelo. `idf.py flash` graba la carpeta `fs/` en la partición `storage` y el firmware lee `/cfg/config.json` al arrancar.
3. **La configuración embebida se usa solo si `/cfg/config.json` no existe.** Cualquier otro problema detiene el equipo: la partición LittleFS no monta, el archivo está vacío, no se puede leer, supera el límite o es inválido. Entonces **no arranca nada** (ni encuesta, ni red, ni MQTT), el LED parpadea en rojo y el log repite cada 10 s, con nivel ERROR, el motivo, por ejemplo:
   - `/cfg/config.json: 140000 bytes; el máximo es 131072 (CONFIG_GW_CONFIG_MAX_BYTES)`;
   - `partición LittleFS 'storage' no montable (ESP_FAIL)`;
   - `devices[0].tags[1].writable: solo las áreas coil y holding son escribibles`.

   Los errores semánticos llevan la **ruta JSON** del valor (`devices[1].tags[2].type: valor no válido (se esperaba bool|uint16|…)`, `devices[0].tags[0].scal: clave desconocida`). Los de sintaxis llevan **línea y columna**, contadas en caracteres a partir del puntero de error de cJSON (`JSON mal formado en la línea 5, columna 3: sintaxis no válida`); también se rechaza el contenido que sobra después del documento. Las **claves duplicadas** en cualquier objeto se rechazan (`mqtt.host: clave duplicada`): cJSON las acepta y sus búsquedas solo ven la primera.
4. **Límite de tamaño: 128 KiB** (`CONFIG_GW_CONFIG_MAX_BYTES`, menú `Gateway: storage`). El máximo del esquema (16 dispositivos, 256 tags con todos los campos) ocupa unos 46 KB en JSON compacto y 82 KB con sangría de 2 espacios. Al arrancar se asignan el búfer del archivo y el árbol de cJSON (unos 200 KB para 256 tags), **en PSRAM** si la hay; se liberan antes de arrancar las tareas. Sin PSRAM (QEMU) el límite real es el heap interno libre, y quedarse sin memoria se informa como tal.

Opciones de compilación en `idf.py menuconfig`, en los menús `Gateway: ...`:

- polaridad de las DI y de las DO (los pines son fijos, en `gw_bsp`);
- prioridades y núcleos de las tareas;
- lecturas en bloque y hueco máximo;
- FC15/FC16 forzadas;
- backoff MQTT;
- espera de SNTP;
- tamaños de cola.

**LED de estado** (de mayor a menor prioridad):

| Estado | Patrón |
|---|---|
| Configuración inválida | Rojo, parpadeo de 4 Hz |
| Sin red (sin IP) | Rojo fijo |
| Sin broker | Ámbar, parpadeo de 1 Hz |
| Dispositivo Modbus fuera de línea | Verde y naranja alternados (1 Hz) |
| Operando | Verde fijo |

## 4. Verificación (evidencia real)

### 4.1 QA-1: tests de host (CMake + Unity v2.6.1 + CTest)

```bash
cmake -S idf/test/host -B build-host && cmake --build build-host -j3 && ctest --test-dir build-host
```

Resultado: **10/10 ejecutables y 68/68 casos OK**, compilados con `-Wall -Wextra -Werror -Wconversion`.

| Suite | Casos | Qué cubre |
|---|---|---|
| `test_mb_rtu` | 6 | CRC (vectores de la especificación Modbus), tramas FC01–06/15/16, longitud de respuesta, excepciones, eco erróneo |
| `test_mb_master` | 8 | t3,5 y retardo entre tramas, timeout y reintentos, CRC malo, unidad equivocada, excepción sin reintento |
| `test_conv` | 5 | Mapeo de datatypes §6, int16/uint16/int32/uint32/float32, ABCD/CDAB, escala inversa (12,3 → 123), rangos |
| `test_logic` | 6 | RBE y deadband contra el último valor reportado (deriva), validez y NaN, debounce, backoff 1–60 s, fuera de línea |
| `test_config` | 11 | Ejemplo §6 literal, JSON embebido, valores por defecto, reglas del esquema, errores con ruta (`devices[1].tags[2].type`), sintaxis con línea y columna (también con UTF-8 y contenido sobrante), claves duplicadas, límites 16/64/256, documento máximo (256 tags, > 40 KB), asignador del árbol y falta de memoria |
| `test_plan` | 2 | Lecturas en bloque, solapes, huecos, límite de 125 registros |
| `test_spb` | 7 | Ida y vuelta de todos los tipos, formato wire (Int16 con extensión de signo), NDEATH sin seq, peor caso de DBIRTH en el búfer |
| `test_node` | 8 | NBIRTH y DBIRTH (nombre, alias, datatype, valor; alias únicos), DDEATH, DDATA por excepción, seq compartido con vuelta a 0, rebirth, NCMD DO/Reboot, DCMD por nombre y alias, descartes |
| `test_poll` | 9 | Encuesta contra un esclavo simulado, fuera de línea tras N ciclos, excepción, un bloque sin respuesta (ciclo fallido, tags sin refrescar, DDEATH, sin vuelta a línea con el ciclo incompleto), contadores por dispositivo, planificación, escritura FC06 con relectura, FC15/FC16 forzadas |
| `test_dout` | 6 | Polaridad de 8 canales (activa en bajo, en alto y mixta), arranque del TCA9554 (latch OFF antes de CONFIG, también en arranque en caliente), DO/3 → pin bajo y relectura, errores de bus |

Los tests de host descargan Unity y cJSON con `FetchContent`, desde GitHub y con tag fijo.

### 4.2 Interoperabilidad Sparkplug

```bash
build-host/spb_dump /tmp/spb && python3 idf/tools/spb_decode.py --check /tmp/spb
```

`spb_dump` genera NDEATH, NBIRTH, DBIRTH, DDATA y NDATA reales del motor. `spb_decode.py` los compila con `grpc_tools.protoc` a partir del `sparkplug_b.proto` oficial de eclipse/tahu (copiado sin cambios en `components/gw_core/proto/`) y los decodifica con **Google protobuf 7.36**.

Resultado: **5 payloads decodificados, 0 errores estructurales**. Además se decodificaron los 36 mensajes capturados en el E2E.

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
- **mosquitto 2.0.18** privado (copia renombrada del binario) y un respondedor SNTP propio en el UDP 123. El harness solo envía señales a los procesos que arrancó él.
- **Aislamiento de red.** El harness se mueve a un *network namespace* propio, con solo la interfaz de loopback (necesita `CAP_SYS_ADMIN`, por ejemplo root en un contenedor). Todos los puertos, también el UDP 123, son privados de la corrida, así que dos corridas en paralelo no chocan. Sin ese permiso (o con `--no-isolate`) usa el loopback del host con los puertos de `--mqtt-port`, `--modbus-port` y `--console-port` (o `E2E_MQTT_PORT`, `E2E_MODBUS_PORT`, `E2E_CONSOLE_PORT`); por defecto 18831, 15020 y 15555. Los argumentos extra de `run_e2e.sh` pasan a `e2e.py`.
- El harness escribe en la imagen bajo prueba su propio `config.json`, con el puerto MQTT elegido. Para los arranques de configuración genera variantes de la imagen con otra partición `storage` (con `littlefs-python`, el mismo que usa la build) o con la partición borrada. El límite de tamaño se lee de `build-qemu/sdkconfig` (`CONFIG_GW_CONFIG_MAX_BYTES`).
- "No arrancó nada" se comprueba con una señal positiva: la línea `nothing is started` y, 6 s después, la respuesta de `status` del shell (`nothing is running`). La ausencia de logs de arranque y de NBIRTH queda como comprobación adicional.

Configuración usada: `tools/e2e/fs/config.json` (keepalive de 10 s).

Resultado: **41/41 comprobaciones OK**.

| Criterio | Resultado medido |
|---|---|
| CFG-1 / CFG-2: arranques con `config.json` de 131 073 B, vacío, con error de sintaxis, con error de esquema y con la partición borrada; y sin archivo | OK: en los cinco primeros no arranca nada (ni UART RS485, ni red, ni MQTT, 0 NBIRTH), el LED pasa a error y el log ERROR da el motivo (`… 131073 bytes; el máximo es 131072`, `línea 5, columna 3`, `devices[0].tags[0].type`, `no montable`). Sin archivo: configuración embebida |
| §7.1 NBIRTH con seq = 0 y bdSeq; al matar la conexión TCP (SIGKILL a QEMU) el broker publica el NDEATH con el mismo bdSeq | OK: NBIRTH a 1,9 s del arranque; NDEATH con el bdSeq de la sesión |
| Reinicio del broker (SIGTERM, 9 s parado) | OK: mosquitto publica el will (NDEATH, bdSeq de la sesión) al cerrarse; backoff 1000 → 1851 → 3745 → 7181 ms con bdSeq nuevo en cada intento; NBIRTH (seq 0, bdSeq nuevo) y DBIRTH de los dos equipos (seq 1 y 2) |
| Broker congelado (SIGSTOP 20 s > 1,5 × keepalive) | OK: el gateway declara muerta la sesión por keepalive a los 10,0 s (`No PING_RESP`); tras SIGCONT llegan los will de la sesión muerta y del intento hecho durante el congelamiento, y luego NBIRTH con bdSeq nuevo y los dos DBIRTH; la sesión nueva sigue viva |
| §7.2 Esclavo apagado → DDEATH; encendido → DBIRTH | OK: DDEATH en 3,05 s (cota 5,8 s); DBIRTH 0,67 s después de volver |
| §7.3 DI → NDATA en menos de 100 ms; NCMD DO/3 = true → NDATA | OK: 23 ms (incluye 20 ms de debounce); NDATA alias 22 = true (pines simulados con la polaridad de `gw_bsp`) |
| §7.4 DCMD Setpoint = 12,3 → holding 10 = 123 por FC06 y DDATA | OK: el esclavo registró `(6, 10, [123])`; DDATA 12,3 |
| §7.5 NCMD Rebirth → NBIRTH (seq 0, mismo bdSeq) y todos los DBIRTH | OK |
| §7.6 Todos los payloads se decodifican con el proto oficial | OK: 36 mensajes, 0 errores |
| CFG-6 shell en la consola; REL-5 | OK: `devices` da `Meter1 unit 1 online cycles 2 failed 0 \| ok 6 …` y, con el esclavo apagado, `offline … failed 3 \| … timeout 9`; `config` da el resumen; `status` responde con la configuración válida (`config … ok`, `MQTT connected`, `Modbus 2/2 devices online`) y también con la configuración rechazada. NBIRTH `Node Info/Reset Reason` = `power-on` en el primer arranque y `software` tras NCMD Reboot |
| Extras | OK: DDATA solo por excepción, solo alias, deadband 0,1 respetada; DCMD por alias (FC05) y DCMD sobre métrica de solo lectura descartado con log; bdSeq persistido en NVS tras un corte de energía; NCMD Reboot publica NDEATH QoS 1 y **un solo** NDEATH (will descartado); DBIRTH de 2684 B (mayor que el búfer de 2 KB de esp-mqtt); 60 registros leídos con una sola FC03; timestamps UTC vía SNTP; continuidad de seq |

**Lo que QEMU no cubre**, y por qué la build de emulador difiere (`sdkconfig.qemu`):

- **W5500.** No está emulado; se usa `open_eth`.
- **TCA9554, GPIO y RMT.** No están emulados; se usa `CONFIG_GW_IO_SIMULATED`: DO en RAM, DI con el comando `di` del shell y LED solo en el log.
- **Modo RS485 de la UART.** `uart_wait_tx_done` hace `assert` por `TX_DONE`; se usa UART normal.
- **PSRAM octal.** QEMU da *segfault*; se deshabilita.
- **Flash QIO.** Las lecturas de la app llegan desplazadas 2 bytes; se usa DIO.

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
| REL-3 | Implementado | `esp_log` en todos los módulos | Q |
| QA-1 | Implementado | `test/host/` | 68 casos |
| QA-2 | Implementado | — | 0 warnings; tamaño en §3.2 |

**Fuera del MVP**, implementado total o parcialmente:

- **MB-11** Parcial: bloques con hueco máximo global por Kconfig, no por dispositivo.
- **MB-13** Parcial: excepciones decodificadas y en el log; contadores por dispositivo (ciclos, ciclos fallidos e intentos ok / timeout / CRC / trama / excepción) en el shell y en el log de fuera de línea, no publicados por Sparkplug.
- **CFG-6** Parcial: shell de diagnóstico de solo lectura en la consola (`status`, `devices`, `config`; `help` lista los comandos). Funciona también con la configuración rechazada. No cambia la configuración.
- **MB-14** Parcial: escrituras antes del siguiente sondeo, no entre bloques.
- **IO-3** Parcial: DO a OFF al reiniciar, sin el resto del modo seguro.
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
- el arranque con flash QIO.

La build de QEMU difiere de la de placa en esos puntos (`sdkconfig.qemu`).

### 6.2 Configuración

- **Configuración inválida o ilegible = nada arranca.** La SPEC pide "no arrancar la encuesta". Aquí tampoco arrancan la red ni MQTT, porque sin una configuración válida no hay a qué conectarse. Recuperarse requiere regrabar (no hay web en el MVP); el comando `status` del shell muestra el motivo.
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

### 6.6 Persistencia y alias

- El bdSeq se escribe en NVS en cada intento de conexión. Desgaste estimado: décadas con un intento por minuto.
- Los alias dependen del orden de los tags en la configuración.

### 6.7 DI y DO

- Una DI que cambia más rápido que el debounce se pierde.
- El timestamp de NDATA es el del lote de cambios, no el de cada bit.
- Las DO pasan a OFF en cada arranque (en frío, en caliente, por watchdog o por panic) **antes de cargar la configuración**, así que también quedan en OFF si la configuración se rechaza. El TCA9554 no se reinicia con el ESP32 y conserva el latch; por eso el firmware lo reescribe con el nivel OFF antes de pasar los pines a salida.
- La polaridad de las DO (activa en bajo) se dedujo del demo y de ESPHome, sin esquemático ni placa. Si la medición dice lo contrario, basta con `CONFIG_GW_DO_ACTIVE_LOW=n`.

### 6.8 Pruebas y entorno

- La versión de firmware lleva `.dirty` hasta que el coordinador haga el commit.
- Los tests de host necesitan red la primera vez (FetchContent de Unity y cJSON).
- Mientras corría el E2E, otro proceso del host compartido ocupó el UDP 123 y terminó el mosquitto por nombre. Por eso el harness usa una copia renombrada del broker y corre en su propio *network namespace* (§4.3). Sin aislamiento, si el UDP 123 está ocupado el check TIME-1 falla: exige que haya respondido el SNTP del harness.
- **pymodbus se fijó en 3.8.6**: la 3.16 cambió el API del datastore (SimDevice).
- `idf.py` no acepta `-j`: la build usó el paralelismo por defecto de ninja.
- Hay dos componentes de terceros: nanopb (vendorizado) y `joltwallet/littlefs`. El paquete `livekit/nanopb` del registro se descartó por un defecto de ABI. `espressif/esp_io_expander_tca9554` se descartó: carga el latch con 0xFF, que con las DO activas en bajo es justamente OFF, pero no permite probar en host la secuencia ni la polaridad (`gw_dout`). La viabilidad (`docs/feasibility-idf.md`, IO-2) lo descartaba suponiendo, por error, DO activas en alto.
