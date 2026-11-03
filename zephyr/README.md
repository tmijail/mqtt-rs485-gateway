# Gateway RS485 (Modbus RTU) → MQTT Sparkplug B 3.0 con Zephyr RTOS

Implementación del MVP de `docs/SPEC.md` (§5) sobre **Zephyr v4.5.0** para la placa Waveshare
**ESP32-S3-POE-ETH-8DI-8DO**. El estudio de viabilidad de todos los requisitos está en
`docs/feasibility-zephyr.md`.

> **Estado, sin adornos:** el firmware **compila sin un solo warning** para el ESP32-S3 con una
> definición de placa propia. Toda la lógica (Sparkplug, Modbus, configuración, E/S) está probada en
> el host con **32 tests unitarios** y una **prueba de punta a punta de 30 comprobaciones** en
> `native_sim` contra mosquitto y un esclavo pymodbus. **Ninguna línea se ejecutó en la placa real**:
> no había hardware. El W5500, la UART RS485 con DE en GPIO21, el TCA9554, el LED por RMT y el
> watchdog de hardware se ejercitan solo en compilación.

## 1. Arquitectura

```
                ┌──────────────────────────── modelo compartido (model.c) ────────────────────────────┐
                │ tags[256] {valor, último reportado, ts, pending}   devs[16] {online, generación, ...} │
                │ io {DI, DO}       bits sucios atómicos: dispositivos (32 bits) y nodo                  │
                └───────▲──────────────────────────▲───────────────────────────────▲─────────────────┘
          escribe valores│ y online/offline   escribe DI/DO│                    lee y publica │
                ┌───────┴────────┐           ┌──────┴───────┐   eventfd (wake)   ┌─────────┴─────────┐
 RS485 ◄──────► │  mb_poller     │           │  local_io    │ ─────────────────► │  mqtt_link        │ ◄──► broker
 (UART1, DE 21) │  dueño único   │           │  1 kHz DI    │                    │  + spb_node       │      MQTT
                │  del bus       │ ◄─────────┼──────────────┼── k_msgq DCMD ──── │  (Sparkplug B)    │
                └────────────────┘           │  LED estado  │ ◄─ set_output ──── │                   │
                                             └──────────────┘    (NCMD DO/n)     └───────────────────┘
                netmgr: DHCP o IP estática, eventos L4 de conn_mgr, SNTP periódico
```

| Hilo | Prioridad | Pila | Watchdog | Qué hace |
|---|---|---|---|---|
| `local_io` | preempt 2 | 1,5 KB | sí | Muestrea las 8 DI a 1 kHz (`k_timer`), aplica el debounce, refresca el LED cada 50 ms |
| `mb_poller` | preempt 5 | 3 KB | sí | **Único dueño del bus RS485 (MB-16).** Encuesta cada dispositivo según su `poll_ms` y atiende las escrituras DCMD de su cola antes de cada transacción |
| `mqtt_link` | preempt 7 | 4 KB | sí | Único usuario del cliente MQTT: conexión, LWT, backoff, suscripción, `poll()` sobre el socket y el eventfd; llama a `spb_node` |
| `netmgr` | preempt 8 | 2,5 KB | sí | Direccionamiento, estado de la red (`conn_mgr`), SNTP (TIME-1) |
| Zephyr | — | — | — | RX del W5500, workqueue del sistema (procesa las tramas Modbus recibidas), shell y logging |

**Flujo de datos sin colas que crezcan (MQTT-7, REL-2).** Los productores (poller y E/S) escriben el
*estado* en el modelo bajo un mutex, marcan un bit sucio atómico y despiertan a `mqtt_link` con un
eventfd. `spb_node` concilia "lo que el host sabe" con "lo que hay":

- dispositivo en línea sin BIRTH, o con otra generación → DBIRTH;
- dispositivo fuera de línea con BIRTH vigente → DDEATH;
- dispositivo sucio → DDATA con las métricas `pending`.

Un broker lento solo retrasa el siguiente DATA, que lleva los valores más recientes: la memoria nunca
crece. La única cola es la de DCMD → bus (`k_msgq`, `CONFIG_GW_WRITE_QUEUE_LEN` = 16). Si se llena,
el comando se rechaza y queda en el log. Todo es estático y está acotado por Kconfig
(`CONFIG_GW_MAX_DEVICES` = 16, `CONFIG_GW_MAX_TAGS_PER_DEVICE` = 64, `CONFIG_GW_MAX_TAGS` = 256, buffer
de payload de 8 KB). No hay `malloc` en la encuesta ni en la publicación.

**Sparkplug.** nanopb sobre el `sparkplug_b.proto` oficial de Eclipse Tahu (`proto/`). Las métricas se
emiten con callbacks (`pb_encode_submessage`), sin estructuras intermedias del tamaño del payload.
`proto/sparkplug_b.options` omite las partes recursivas (Template, DataSet y PropertySet).

**Plan de aliases (SPB-8):**

| Métrica | Alias |
|---|---|
| `bdSeq` | 1 |
| `Node Control/Rebirth` | 2 |
| `Node Control/Reboot` | 3 |
| `Node Info/Firmware` | 4 |
| `Node Info/Hardware` | 5 |
| `DI/1…8` | 11–18 |
| `DO/1…8` | 21–28 |
| Tag *n* (índice global en la configuración) | 1000 + *n* |

**Modbus.** Se usa el **subsistema Modbus de Zephyr** (rol cliente) para el framing RTU, el CRC, el
silencio de 3,5 caracteres y el pin DE (`de-gpios`). La política es nuestra (`mb_poller.c`):
reintentos, retardo entre transacciones, conversión, detección de fuera de línea y la cola de
escrituras. Las reglas son estas:

- Un ciclo falla en cuanto una petición queda sin respuesta tras sus reintentos. Una excepción
  Modbus cuenta como respuesta.
- Tras `offline_after` ciclos fallidos seguidos, el dispositivo pasa a fuera de línea.
- Mientras está fuera de línea solo se sondea su primer tag. Si responde, se lee el dispositivo
  entero y se publica DBIRTH.
- Las escrituras se confirman releyendo el tag y se reportan siempre, aunque no superen la banda
  muerta.

### Archivos

| Ruta | Contenido |
|---|---|
| `boards/waveshare/esp32s3_poe_eth_8di_8do/` | **Definición de placa propia** (HWMv2): DTS con el mapa de pines real, particiones de 16 MB y pinctrl |
| `boards/esp32s3_poe_eth_8di_8do_procpu.{conf,overlay}` | Kconfig de la aplicación para la placa (W5500, DHCP, I²C, LED) y el `fstab` de LittleFS |
| `boards/native_sim.{conf,overlay}` | Simulación: sockets del host (NSOS), RS485 en un tty del host, GPIO emulados |
| `src/main.c` | Arranque y watchdog |
| `src/app_config.c` | Elige la configuración: archivo en LittleFS o la embebida por defecto |
| `src/json_reader.c`, `src/config.c` | Lector JSON y validador del esquema §6 (CFG-1) |
| `src/model.c` | Estado compartido, RBE y deadband |
| `src/mb_poller.c` | Maestro Modbus, dueño del bus |
| `src/convert.c` | Tipos, orden de palabras y escala |
| `src/sparkplug.c` | Codec nanopb y topics |
| `src/spb_node.c` | Semántica Sparkplug: BIRTH, DEATH, DATA, seq, aliases, comandos |
| `src/mqtt_link.c`, `src/backoff.h` | Sesión MQTT, LWT y backoff |
| `src/bdseq.c` | bdSeq persistente (settings + ZMS) |
| `src/netmgr.c`, `src/gw_time.c` | Red y SNTP |
| `src/local_io.c`, `src/debounce.c`, `src/status_led.c` | DI, DO y LED |
| `src/gw_shell.c` | Comando `gw status` (y `gw di` solo en simulación) |
| `config/default_config.json` | Configuración por defecto (el ejemplo de §6) |
| `tests/unit/` | ztest en `native_sim` (QA-1) |
| `tests/e2e/` | Prueba de punta a punta (QA-3): `run_e2e.py`, `modbus_slave.py`, `e2e_config.json` |
| `scripts/decode_samples.py` | Decodifica payloads con el proto oficial y Python `protobuf` |
| `scripts/mk_config_image.py` | Genera una imagen LittleFS con `config.json` para grabarla en la partición `lfs` |
| `west.yml` | Manifiesto west mínimo con el que se construyó |

## 2. Datos de la placa (los marcados "verificar" en el spec)

No hay esquemático público de esta placa. Ni el wiki ni la página de recursos lo enlazan, y las URL
candidatas en `files.waveshare.com` devuelven 404. Fuentes usadas:

- **[W]** wiki de Waveshare `ESP32-S3-POE-ETH-8DI-8DO`;
- **[D]** demo oficial `ESP32-S3-POE-ETH-8DI-8DO-Demo.zip` (Arduino, `MAIN_MQTT_ALL`);
- **[E]** configuración de ESPHome de la placa, en devices.esphome.io.

| Dato | Lo que se encontró | Confianza |
|---|---|---|
| Expansor de las DO | **TCA9554PWR en 0x20**: [W] (FAQ) y [D] (`WS_TCA9554PWR.h`: `#define TCA9554_ADDRESS 0x20`). [E] usa `pca9554` en 0x20, que es compatible. DO1…8 = EXIO1…8 = pines 0…7 | Alta |
| Polaridad de las DO | La demo arranca con `TCA9554PWR_Init(0x00, 0xFF)`, es decir, todas las salidas a nivel alto y `Dout_Flag = {1,…}`. [E] declara las 8 salidas con `inverted: true`. Conclusión: **nivel bajo = salida activa** (`GPIO_ACTIVE_LOW`). Además, al encender, el TCA9554 deja los pines como entradas (alta impedancia), así que las salidas arrancan apagadas | Media |
| RTC | **PCF85063 en 0x51**: [D] (`WS_PCF85063.h`: `PCF85063_ADDRESS 0x51`); el wiki de la variante 8RO dice PCF85063ATL. INT en GPIO40 | Alta |
| Polaridad de las DI | [D] configura `INPUT_PULLUP`, y su lógica DI→DO asume reposo en 1 (`DIN_Data_Old = 0xFF`). [E] declara `INPUT_PULLUP` + `inverted: true`. Conclusión: **entrada energizada = GPIO a nivel bajo** (`GPIO_PULL_UP \| GPIO_ACTIVE_LOW`) | Media-alta |
| Buzzer | **Pasivo**: [D] lo maneja con LEDC PWM (`ledcAttach(GPIO46, 1000 Hz, 8 bits)`) y [E] con `ledc` + `rtttl`. Un buzzer activo no necesitaría PWM | Media-alta |
| RS485 DE | GPIO21 = RTS: [W] (tabla de pines) y [D] (`setPins(-1,-1,-1, TXD1EN=21)` + `UART_MODE_RS485_HALF_DUPLEX`). En IDF ese modo pone RTS en alto mientras transmite, así que **DE es activo en alto** | Media-alta |
| LED RGB | GPIO38. [D] llama a `neopixelWrite(pin, verde, rojo, azul)` con los parámetros cambiados de sitio, lo que indica que el LED espera los bytes en **orden RGB**, no GRB. Se configuró `color-mapping` R, G, B | Media |
| Consola USB | El wiki no dice si el USB-C va al USB nativo (Serial/JTAG) o a un CH34x. Menciona un jumper USB-OTG, lo que sugiere el USB nativo. Se eligió `usb_serial`; si no funciona, cambiar `zephyr,console` a `&uart0` (GPIO43/44) | Baja-media |
| W5500 | MOSI 13, MISO 14, SCLK 15, CS 16, INT 12, RST 39: [W] y [D] (`WS_ETH.h`) | Alta |
| microSD | CLK 48, CMD 47, D0 45 (1 bit): [W] y [D] | Alta |

## 3. Compilar, grabar y configurar

### Entorno (fuera del repositorio)

```bash
python3 -m venv ~/sdk/zephyr/.venv && . ~/sdk/zephyr/.venv/bin/activate && pip install west
mkdir -p ~/sdk/zephyr/ws/manifest && cp zephyr/west.yml ~/sdk/zephyr/ws/manifest/
cd ~/sdk/zephyr/ws && west init -l manifest && west update --narrow -o=--depth=1
pip install -r zephyr/scripts/requirements-base.txt -r zephyr/scripts/requirements-build-test.txt \
            -r zephyr/scripts/requirements-run-test.txt 'esptool>=5.0.2' protobuf
# Zephyr SDK 1.0.1, solo la toolchain Xtensa del ESP32-S3:
west sdk install -t xtensa-espressif_esp32s3_zephyr-elf   # o a mano desde las releases de sdk-ng
apt install protobuf-compiler device-tree-compiler gcc-multilib   # protoc (nanopb) y native_sim
```

En este entorno, la API de GitHub de `west sdk install` y los blobs de Wi-Fi (`west blobs fetch`)
devolvieron 403. El SDK se descargó de `github.com/zephyrproject-rtos/sdk-ng/releases/download/v1.0.1/`
y se comprobó con `sha256.sum`. El MVP **no necesita blobs**: no usa Wi-Fi ni BLE.

### Firmware para la placa (QA-2)

```bash
cd ~/sdk/zephyr/ws
west build -p -b esp32s3_poe_eth_8di_8do/esp32s3/procpu <repo>/zephyr -d build-gw -o=-j3
west flash -d build-gw          # esptool por USB; la imagen va a 0x0 (ESP_SIMPLE_BOOT)
west espressif monitor          # o cualquier terminal en el puerto USB
```

Resultado del build (Zephyr 4.5.0, GCC 14.3, `-Os`): **0 warnings** en todo el build (`grep -c warning:`
del log = 0).

| Región | Uso | Total | % |
|---|---|---|---|
| FLASH (imagen) | 422 308 B | 16 MB | 2,5 % |
| DRAM (`dram0_0_seg`) | 217 480 B | 399 108 B | 54,5 % |
| IRAM (`iram0_0_seg`) | 66 568 B | 415 492 B | 16,0 % |

Los símbolos propios más grandes en RAM son el buffer de lectura de configuración (32 KB, solo se usa
en el arranque), la configuración parseada (17,5 KB), el estado del nodo Sparkplug con el buffer de
payload (10 KB) y el estado de tags (8 KB).

### Configuración (CFG-1, CFG-2)

1. **Por defecto embebida:** `config/default_config.json`. Se puede embeber otra al compilar con
   `-- -DGW_CONFIG_FILE=/ruta/mi_config.json`.
2. **Archivo en LittleFS:** si existe `/lfs/config.json`, se usa en lugar de la embebida. Para
   grabarlo sin herramientas en el equipo:

   ```bash
   pip install littlefs-python
   python3 scripts/mk_config_image.py mi_config.json -o lfs.bin       # partición de 3 MB
   esptool --chip esp32s3 write_flash 0xc20000 lfs.bin
   ```

   El mismo mecanismo se probó en `native_sim` con
   `--size 512K --flash-file flash.bin --flash-size 2M --offset 0x100000`. El log mostró
   `configuration from /lfs/config.json` y el NBIRTH salió con el `edge_node_id` del archivo.
3. **Configuración inválida:** el error indica la ruta JSON y, si es de sintaxis, la línea y la
   columna. Por ejemplo, `devices[0].tags[1].type: invalid value 'int64'` o
   `mqtt: line 1, col 21: trailing comma`. **No se arranca la encuesta ni MQTT.** El LED parpadea
   en rojo rápido y `gw status` muestra `config: INVALID`. Se comprobó en simulación con un archivo
   inválido en LittleFS.

Reglas que valida `config.c`, además de las del §6:

- No se admiten miembros desconocidos.
- Los rangos son: baud de 1200 a 115200, `unit_id` de 1 a 247, `poll_ms` mayor o igual que 50,
  `keepalive_s` de 5 a 3600, y la dirección más los registros del tipo no puede pasar de 65535.
- No puede haber nombres de dispositivo ni `unit_id` repetidos, ni nombres de tag repetidos dentro
  de un dispositivo.
- Los identificadores Sparkplug no pueden contener `/`, `+` ni `#`.
- Si `dhcp` es false, `ip`, `netmask` y `gateway` son obligatorios.
- Se aplican los límites de 16 dispositivos, 64 tags por dispositivo y 256 tags en total.
- `units` se acepta y se ignora (engUnit es SPB-16).
- `primary_host_id` se acepta, pero si no está vacío se avisa de que SPB-13 no está implementado.

### LED de estado (IO-5)

| Condición (de mayor a menor prioridad) | LED |
|---|---|
| Configuración inválida | rojo, parpadeo rápido (4 Hz) |
| Sin red (sin enlace o sin IP) | rojo, parpadeo lento (1 Hz) |
| Red OK, sin broker | amarillo, parpadeo lento |
| Algún dispositivo Modbus fuera de línea | naranja fijo |
| Operando | verde fijo |

## 4. Pruebas y evidencia

### QA-1: tests unitarios (ztest en `native_sim`)

```bash
west twister -T <repo>/zephyr/tests/unit -p native_sim -j 3
#  -> 32 of 32 executed test cases passed (100.00%) ... with no warnings
```

| Suite | Qué cubre |
|---|---|
| `config` | El ejemplo exacto del §6, los valores por defecto, IP estática, escapes y UTF-8, **32 mensajes de error** comprobados texto a texto, y límites de 256 tags, 64 por dispositivo y 16 dispositivos |
| `convert` | int16/uint16/int32/uint32/float32, `big`/`little`, escala y offset, inversa con redondeo y rango (12,3 → 123), ida y vuelta, mapeo de datatypes del §6 |
| `rbe`, `rbe_store` | Deadband en ambos sentidos, igual a la banda (no reporta), NaN, bool, deriva medida desde el último reporte, escrituras forzadas |
| `sparkplug` | Ida y vuelta de todos los tipos (Int16 negativo como complemento a 2 de 32 bits), `is_null`, buffer insuficiente, nombres y strings demasiado largos, payload corrupto, topics y parseo de NCMD/DCMD |
| `spb_node` | NBIRTH (seq 0, bdSeq, 21 métricas completas), DBIRTH, RBE agrupado en un DDATA solo por alias, DDEATH y DBIRTH, rebirth, seq que pasa de 255 a 0, DCMD por nombre y alias (12,3 → 123), descartes, NCMD de DO y Reboot, NDEATH sin seq |
| `debounce`, `backoff`, `status_led` | Glitch de 19 ms rechazado, cambio aceptado a los 20 ms, canales independientes; backoff de 1 a 60 s con jitter; prioridades del LED |

**CRC y framing Modbus:** no son código nuestro (los aporta el subsistema Modbus de Zephyr), así que
no tienen test unitario propio. Su interoperabilidad se demuestra en la prueba de punta a punta
contra pymodbus, que es una implementación independiente: FC01, 02, 03, 04, 05, 06 y 16.

### Comprobación Sparkplug con el proto oficial

```bash
build-unit/zephyr/zephyr.exe > unit.log    # los tests de spb_node imprimen "SPB_SAMPLE <topic> <hex>"
python3 scripts/decode_samples.py unit.log
#  -> 19/19 payloads decoded and valid   (NBIRTH, DBIRTH, DDATA, NDATA, DDEATH, NDEATH)
python3 scripts/decode_samples.py --bin <out>/sample_*.bin    # payloads reales capturados en la e2e
#  -> 6/6 payloads decoded and valid
```

Extracto real (DBIRTH capturado del broker en la prueba de punta a punta, decodificado con
`sparkplug_b_pb2` generado por `protoc` desde el proto de Tahu):

```
metrics { name: "Voltage"  alias: 1000 timestamp: 1793708414398 datatype: 9  float_value: 230.0 }
metrics { name: "Setpoint" alias: 1001 timestamp: 1793708414423 datatype: 9  float_value: 0.0 }
metrics { name: "Run"      alias: 1002 timestamp: 1793708414444 datatype: 11 boolean_value: false }
metrics { name: "Energy"   alias: 1003 timestamp: 1793708414471 datatype: 7  int_value: 305419896 }
seq: 1
```

### QA-3: prueba de punta a punta en `native_sim`

```
zephyr.exe (native_sim) ──sockets del host (NSOS)──► mosquitto :18830 ◄── run_e2e.py (paho + proto oficial)
     └─ UART native_tty ─ /tmp/gw-rs485 ══ par pty de socat ══ /tmp/gw-slave ─ modbus_slave.py (pymodbus 3.16)
     DI: gpio-emul, movidas con "gw di" por stdin. DO: gpio-emul en lugar del TCA9554. SNTP: servidor en la prueba.
```

```bash
apt install mosquitto socat; pip install paho-mqtt pymodbus protobuf
west build -p -b native_sim <repo>/zephyr -d build-gw-sim -- \
     -DGW_CONFIG_FILE=<repo>/zephyr/tests/e2e/e2e_config.json
sudo python3 tests/e2e/run_e2e.py --exe build-gw-sim/zephyr/zephyr.exe --out /tmp/e2e
# (root: el servidor SNTP de la prueba escucha en 127.0.0.1:123)
```

Resultado (dos ejecuciones seguidas, ambas **30/30**):

```
PASS  REL-6 boot to NBIRTH: 0.01 s
PASS  SPB-4 NBIRTH seq=0, QoS 0, not retained
PASS  SPB-4 NBIRTH metrics complete: 21 metrics, missing=[]
PASS  FW-4 firmware version + git hash: '0.1.0+1be73834904c'
PASS  SPB-3 bdSeq is UInt64 (datatype 8)
PASS  TIME-1 NBIRTH timestamp is ms UTC
PASS  SPB-6 DBIRTH with all metrics and values
PASS  MB-3/MB-5 FC01-04, float32 ABCD, uint32 CDAB, discrete
PASS  SPEC 6 datatype mapping: {'Voltage': 9, 'Setpoint': 9, 'Run': 11, 'Energy': 7, 'Alarm': 11}
PASS  SPB-8 aliases unique per node: 26 aliases
PASS  SPB-5 DBIRTH continues seq: seq=1
PASS  SPB-7/SPB-12 deadband 0.1 suppresses +0.05, reports +0.5: 6 slave steps -> 3 DDATA values
PASS  SPB-7 DATA by alias without name, with timestamp
PASS  SPEC 7.3a DI change in NDATA < 100 ms (20 ms debounce): latencies ms: 27.1, 26.5, 25.9
PASS  SPEC 7.3b NCMD DO/3=true confirmed by NDATA: 47 ms
PASS  SPEC 7.4 DCMD Setpoint=12.3 writes 123 with FC06, DDATA confirms
PASS  MB-4 DCMD by alias Run=true writes FC05, DDATA confirms
PASS  MB-4 DCMD uint32 (CDAB) writes FC16, DDATA confirms
PASS  SPB-10 read-only and unknown metrics discarded and logged
PASS  SPEC 7.5 Rebirth: NBIRTH seq=0 (same bdSeq) + all DBIRTH
PASS  IO-2 DO state reported in rebirth
PASS  SPEC 7.2 slave off -> DDEATH within offline_after x poll + timeouts: 2.70 s (bound 3.8 s)
PASS  SPEC 7.2 slave back -> DBIRTH: 0.62 s
PASS  SPB-5 seq +1 (mod 256) across node and device messages, 0 at NBIRTH
PASS  MQTT-2 reconnects after broker restart (backoff <= 60 s): 2.6 s, bdSeq 0 -> 3
PASS  SPEC 7.1 / SPB-2 kill -> broker publishes NDEATH with NBIRTH bdSeq: NDEATH bdSeq=3 NBIRTH bdSeq=3 qos=1
PASS  SPB-3 bdSeq persisted in flash and incremented after reboot: before reboot 3, after 4
PASS  SPB-11 NCMD Reboot publishes NDEATH before disconnecting
PASS  TIME-1 SNTP synchronised: 2/2 boots synchronised
PASS  SPB-1 every payload decodes with the official proto: 33 messages, 0 decode errors
```

Qué **no** cubre la e2e:

- el W5500 y la pila IP nativa: la simulación usa sockets del host, así que DHCP e IP estática no se
  ejecutan;
- la temporización eléctrica de la UART y del DE;
- el TCA9554 y el LED reales;
- el reset por watchdog.

## 5. Estado de cada ID del MVP

**Evidencia:** U = test unitario, E = prueba de punta a punta en `native_sim`, B = solo compila para
el ESP32-S3 (código no ejecutado).

| ID | Estado | Evidencia | Dónde |
|---|---|---|---|
| NET-1 | Implementado | B | `boards/waveshare/.../esp32s3_poe_eth_8di_8do_procpu.dts` (nodo `w5500`), `src/netmgr.c` |
| NET-2 | Implementado | B | `src/netmgr.c` (`configure_ipv4`) |
| NET-3 | Implementado | E (resolver del host vía NSOS con IP numérica) / B | `src/mqtt_link.c` (`resolve_broker`), `src/netmgr.c` (DNS estático) |
| TIME-1 | Implementado | E | `src/gw_time.c`, `src/netmgr.c`, `src/mqtt_link.c` (espera de hasta 5 s) |
| MQTT-1 | Implementado | E (sin usuario ni contraseña) | `src/mqtt_link.c` |
| MQTT-2 | Implementado | U + E | `src/backoff.h`, `src/mqtt_link.c` |
| MQTT-3 | Implementado | E | `src/mqtt_link.c` (`prepare_client`) |
| MQTT-7 | Implementado | Diseño + U | `src/model.c`, `src/mb_poller.c` (`k_msgq`) |
| SPB-1 | Implementado | U + E (proto oficial) | `proto/`, `src/sparkplug.c` |
| SPB-2 | Implementado | E | `src/mqtt_link.c`, `src/spb_node.c` (`spb_node_encode_death`) |
| SPB-3 | Implementado | E (persistencia tras reinicio) | `src/bdseq.c` |
| SPB-4 | Implementado | U + E | `src/spb_node.c` (`publish_nbirth`) |
| SPB-5 | Implementado | U (paso de 255 a 0) + E | `src/spb_node.c` |
| SPB-6 | Implementado | U + E | `src/spb_node.c` (`service_device`), `src/mb_poller.c` |
| SPB-7 | Implementado | U + E | `src/spb_node.c` (`publish_ddata`, `publish_ndata`) |
| SPB-8 | Implementado | U + E | `src/spb_node.h` (plan de aliases) |
| SPB-9 | Implementado | U + E | `src/spb_node.c` |
| SPB-10 | Implementado | U + E | `src/spb_node.c` (`on_dcmd_metric`), `src/mb_poller.c` (`do_write`) |
| SPB-11 | Implementado | U + E | `src/spb_node.c` (`on_ncmd_metric`), `src/local_io.c`, `src/mqtt_link.c` |
| SPB-12 | Implementado | U + E | `src/model.c` (`model_rbe_exceeds`) |
| MB-1 | Implementado | E (9600 8N1) / B | `src/mb_poller.c` (`mb_poller_start`), `CONFIG_MODBUS_NONCOMPLIANT_SERIAL_MODE` |
| MB-2 | Implementado con la alternativa GPIO | B | DTS: `de-gpios = <&gpio0 21 GPIO_ACTIVE_HIGH>`. El modo RS485 por hardware no se usa (sección 6) |
| MB-3 | Implementado | E (FC01, 02, 03, 04) | `src/mb_poller.c` (`request`) |
| MB-4 | **Parcial** | E (FC05, 06, 16) | `src/mb_poller.c`: FC15 no se emite nunca, cada coil se escribe con FC05 |
| MB-5 | Implementado | U + E | `src/convert.c` |
| MB-7 | Implementado | U + E | `src/convert.c` |
| MB-8 | Implementado | E | `src/mb_poller.c` (`transact`, `inter_frame_delay`); 3,5 caracteres: temporizador RTU del subsistema |
| MB-9 | Implementado | E | `src/mb_poller.c` (`poll_device`) |
| MB-10 | Implementado | E | `src/mb_poller.c` (`poller_main`) |
| MB-16 | Implementado | Diseño + E | `src/mb_poller.c` (un único hilo hace todas las transacciones) |
| IO-1 | Implementado | U (debounce) + E (gpio-emul, menos de 30 ms) | `src/local_io.c`, `src/debounce.c` |
| IO-2 | Implementado | E (gpio-emul) / B (TCA9554) | `src/local_io.c`, DTS (`exio`, `ti,tca9554`) |
| IO-5 | Implementado | U (colores) / B (driver) | `src/status_led.c`, DTS (`rgb_led`, `worldsemi,ws2812-pulse-io` sobre RMT) |
| CFG-1 | Implementado | U (32 errores) + E (archivo inválido) | `src/json_reader.c`, `src/config.c`, `src/main.c` |
| CFG-2 | Implementado | E (imagen LittleFS en `native_sim`) | `src/app_config.c`, `scripts/mk_config_image.py` |
| FW-4 | Implementado | E | `VERSION`, `src/mqtt_link.c` (`APP_VERSION_STRING` + `APP_BUILD_VERSION`) |
| REL-1 | Implementado | B (no se probó que el reset dispare) | `task_wdt` en los 4 hilos, `wdt0` como respaldo (`src/main.c`) |
| REL-2 | Implementado | Diseño | Todo estático (Kconfig `GW_MAX_*`) |
| REL-3 | Implementado | E | Logging de Zephyr con niveles (`CONFIG_GW_LOG_LEVEL`) |
| QA-1 | Implementado | 32/32 | `tests/unit/` (CRC y framing Modbus no son nuestros, ver sección 4) |
| QA-2 | Implementado | 0 warnings | Ver sección 3 |

Además, aunque no forman parte del MVP: **QA-3** está hecho (30/30); **MB-14** está parcial (las
escrituras van antes que la encuesta); **CFG-6** está parcial (`gw status`); **REL-5** está parcial
(el motivo del reset solo se registra en el log); **REL-6** se cumple en simulación.

## 6. Desviaciones y limitaciones conocidas

1. **Nada se probó en hardware.** En particular, la polaridad de las DI y DO, el orden de color del
   LED, la consola en `usb_serial`, el W5500 a 20 MHz y la conmutación de DE se dedujeron de la demo,
   del wiki y de ESPHome (sección 2), no de la placa.
2. **MB-2: no se usa el modo RS485 por hardware de la UART.** Se revisó el código de Zephyr 4.5.0:
   - `uart_esp32.c` admite `hw-rs485-hd-mode`, pero su binding dice que activa **DTR**, no RTS;
   - el driver no conmuta RTS como hace ESP-IDF;
   - el subsistema Modbus (`modbus_serial.c::configure_uart`) fija `flow_ctrl = NONE` al iniciar,
     lo que anularía ese modo.

   DE se maneja por GPIO21 con `de-gpios`. El subsistema lo libera en la ISR al terminar la
   transmisión (`uart_irq_tx_complete`), que es la alternativa que permite el spec.
3. **MB-4: no se emite FC15.** La API del subsistema lo tiene (`modbus_write_coils`), pero el MVP
   escribe un tag por comando. Las coils van siempre con FC05.
4. **El subsistema Modbus registra un `WRN` por cada timeout** (`modbus_core.c`). Con un esclavo
   apagado eso llena el log, a razón de una línea por cada reintento de cada ciclo. Es ajeno al
   código propio, pero visible.
5. **Detección de fuera de línea:** un tag que nunca responde, sin siquiera excepción (algo que
   incumple Modbus), hace fallar todos los ciclos y deja el dispositivo fuera de línea. Es una
   decisión deliberada para detectar antes un equipo apagado (DDEATH en unos 2,7 s con la
   configuración de la e2e).
6. **Calidad de los tags:** si un tag de un dispositivo en línea deja de leerse (por una excepción),
   se mantiene su último valor y no se avisa. La calidad (SPB-16) no está en el MVP. En el DBIRTH,
   los tags nunca leídos van con `is_null`.
7. **Timestamps:** si no hay SNTP, se espera hasta 5 s antes del primer NBIRTH y después se publica
   con un reloj que cuenta desde 1970 al arrancar (el RTC, TIME-2, no está en el MVP). `gw status`
   indica si la hora está sincronizada.
8. **Confirmación de las DO:** el valor confirmado es el estado escrito cuando el TCA9554 reconoce
   (ACK) la transacción I²C. No hay relectura del pin.
9. **SPB-2: NDEATH explícito solo en el reinicio por NCMD.** No hay otra desconexión ordenada en el
   MVP: no hay cambio de configuración en caliente.
10. **`primary_host_id` (SPB-13) se ignora**, con un aviso en el log.
11. **Configuración en el equipo:** no hay interfaz web, REST ni comandos de shell para escribirla.
    Se graba una imagen LittleFS (`scripts/mk_config_image.py` + `esptool`) o se embebe al compilar.
12. **RAM:** el buffer de 32 KB para leer el archivo de configuración solo se usa en el arranque y
    sigue reservado después. La PSRAM de 8 MB está declarada en la DTS pero **no se habilitó**
    (`CONFIG_ESP_SPIRAM`).
13. **Arranque sin MCUboot** (`ESP_SIMPLE_BOOT`), por lo que no hay OTA. Las particiones ya dejan dos
    slots de 6 MB para cuando se haga FW-1.
14. **Simulación:** en una de las primeras ejecuciones se vieron timeouts esporádicos de 200 ms en
    el par pty. Desaparecieron al repetir y nunca afectaron al resultado (hay reintentos).
    Probablemente fue carga del host compartido. No se pudo atribuir con certeza.
15. **Blobs de Wi-Fi:** no se pudieron descargar (HTTP 403 del proxy), así que nada inalámbrico se
    compiló.
