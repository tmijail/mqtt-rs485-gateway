# Especificación de producto — Gateway RS485 (Modbus RTU) → MQTT Sparkplug B

Versión: 1.0 · Fecha: 2026-11-03

## 1. Objetivo

Gateway industrial de borde que:

- encuesta equipos de campo por **RS485 / Modbus RTU**,
- publica sus variables en un broker **MQTT** con **Sparkplug B 3.0**,
- acepta comandos de escritura desde SCADA o MES (DCMD/NCMD),
- y expone sus 8 entradas y 8 salidas digitales locales como métricas Sparkplug.

Al publicar, el gateway hace de *Edge Node* de Sparkplug. Cada esclavo Modbus es un *Device* de Sparkplug.

## 2. Selección de hardware

| Candidato | Programable | Veredicto |
|---|---|---|
| Waveshare RS232/485/422 TO POE ETH (B) | **No**: firmware cerrado de servidor serie; solo se configura (web, herramienta de PC o set de comandos) | Descartado: no es posible implementar Sparkplug B propio |
| **Waveshare ESP32-S3-POE-ETH-8DI-8DO** | **Sí**: ESP32-S3, soportado por ESP-IDF, Arduino y Zephyr | **Elegido** |

### 2.1 Recursos del ESP32-S3-POE-ETH-8DI-8DO

| Recurso | Detalle | Pines |
|---|---|---|
| MCU | ESP32-S3-WROOM-1U-**N16R8**: 2× Xtensa LX7 a 240 MHz, 16 MB de flash, 8 MB de PSRAM octal | — |
| Ethernet | **W5500** por SPI, 10/100 | MOSI 13, MISO 14, SCLK 15, CS 16, INT 12, RST 39 |
| PoE | Módulo IEEE 802.3af integrado | — |
| RS485 | Aislado, half-duplex, con dirección por RTS | TX 17, RX 18, **RTS/DE 21** |
| CAN | Aislado, TWAI | TX 2, RX 3 |
| DI ×8 | Optoaisladas, bidireccionales, aceptan contacto seco o señal activa | GPIO 4–11 (**verificar polaridad**) |
| DO ×8 | Darlington optoaislado, hasta 500 mA en modo sumidero, con diodo de rueda libre | EXIO1–8 de un expansor I²C (TCA9554/PCA9554, **verificar dirección**, típicamente 0x20) |
| RTC | I²C con interrupción (PCF85063, **verificar**) | SDA 42, SCL 41, INT 40 |
| LED RGB | WS2812 | GPIO 38 |
| Buzzer | Activo o pasivo (**verificar**) | GPIO 46 |
| microSD | Modo SPI o SDMMC de 1 bit | D0 45, CMD 47, SCK 48 |
| Botón BOOT | — | GPIO 0 |
| Wi-Fi / BLE | 2,4 GHz, del ESP32-S3 | — |
| Alimentación | 7–36 V por bornera, PoE o USB-C 5 V | — |

> El TCA9554 de las salidas y el RTC comparten el bus I²C de los GPIO 41 y 42. Los datos marcados como **verificar** deben confirmarse con el esquemático o el código demo de Waveshare.

## 3. Convenciones

- Cada requisito tiene un ID con el formato `ÁREA-n`.
- La columna **MVP** indica si el requisito forma parte del producto mínimo (§5).
- Prioridad: **P0** = imprescindible, **P1** = esperado en un producto de buen nivel, **P2** = diferencial.

## 4. Especificación completa

### 4.1 Red (NET)

| ID | Requisito | Prio | MVP |
|---|---|---|---|
| NET-1 | Ethernet por W5500 como interfaz primaria, con detección de enlace (link up/down) y reconexión automática | P0 | ✅ |
| NET-2 | IPv4 por DHCP o estática (IP, máscara, gateway y DNS), configurable | P0 | ✅ |
| NET-3 | Resolución de nombres por DNS para el host del broker | P0 | ✅ |
| NET-4 | Wi-Fi STA como interfaz alternativa o de respaldo cuando cae Ethernet, con prioridad configurable | P1 | |
| NET-5 | Modo AP de Wi-Fi para la puesta en marcha: se activa si no hay configuración válida o con el botón BOOT | P1 | |
| NET-6 | Hostname configurable y anuncio mDNS (`_http._tcp`) | P2 | |
| NET-7 | IPv6 | P2 | |

### 4.2 Tiempo (TIME)

| ID | Requisito | Prio | MVP |
|---|---|---|---|
| TIME-1 | Sincronización por SNTP con un servidor configurable; los timestamps Sparkplug van en ms UTC | P0 | ✅ |
| TIME-2 | Lectura del RTC al arrancar y su actualización tras cada SNTP exitoso, para tener timestamps válidos sin red | P1 | |
| TIME-3 | Métrica `Node Info/Time Synced` que indica si la hora es confiable | P2 | |

### 4.3 MQTT (MQTT)

| ID | Requisito | Prio | MVP |
|---|---|---|---|
| MQTT-1 | Cliente MQTT 3.1.1 sobre TCP con host, puerto, client_id, usuario, contraseña y keepalive configurables | P0 | ✅ |
| MQTT-2 | Reconexión automática con *backoff* exponencial y jitter (de 1 s a 60 s) | P0 | ✅ |
| MQTT-3 | LWT con el NDEATH de Sparkplug (§4.4) | P0 | ✅ |
| MQTT-4 | TLS 1.2 o 1.3 con CA configurable y verificación de hostname | P1 | |
| MQTT-5 | TLS mutuo con certificado y clave de cliente | P1 | |
| MQTT-6 | Lista de brokers con conmutación por falla, en orden | P1 | |
| MQTT-7 | Colas de publicación acotadas: nunca hay alocación dinámica sin límite ante un broker lento | P0 | ✅ |
| MQTT-8 | MQTT 5.0 | P2 | |

### 4.4 Sparkplug B 3.0 (SPB)

**Topics:**

- `spBv1.0/{group_id}/{NBIRTH|NDEATH|NDATA|NCMD}/{edge_node_id}`
- `spBv1.0/{group_id}/{DBIRTH|DDEATH|DDATA|DCMD}/{edge_node_id}/{device_id}`
- `spBv1.0/STATE/{primary_host_id}`

| ID | Requisito | Prio | MVP |
|---|---|---|---|
| SPB-1 | Payload protobuf conforme a `sparkplug_b.proto` de Eclipse Tahu | P0 | ✅ |
| SPB-2 | **NDEATH** registrado como LWT: QoS 1, sin retain, con la métrica `bdSeq` (UInt64). Al desconectarse de forma ordenada se publica NDEATH explícitamente | P0 | ✅ |
| SPB-3 | **bdSeq** de 0 a 255, persistido en memoria no volátil. Se incrementa en cada intento de conexión y el NBIRTH lleva el mismo valor que el LWT de esa conexión | P0 | ✅ |
| SPB-4 | **NBIRTH**: QoS 0, sin retain, `seq`=0, timestamp. Incluye `bdSeq`, `Node Control/Rebirth` (Boolean, escribible), `Node Control/Reboot`, `Node Info/Firmware`, `Node Info/Hardware` y las métricas de E/S local (§4.6), todas con nombre, alias, datatype y valor | P0 | ✅ |
| SPB-5 | **seq** de 0 a 255 que vuelve a empezar, compartido por todos los mensajes del nodo salvo NDEATH | P0 | ✅ |
| SPB-6 | **DBIRTH** por cada dispositivo Modbus en línea, con todas sus métricas y su valor actual. **DDEATH** cuando el dispositivo pasa a fuera de línea (§4.5, MB-9) | P0 | ✅ |
| SPB-7 | **NDATA/DDATA** por excepción: solo se publican las métricas que cambiaron, identificadas por alias y sin nombre, con timestamp. Se agrupan los cambios de un mismo ciclo de encuesta en un único mensaje | P0 | ✅ |
| SPB-8 | **Aliases** únicos de tipo UInt64 por nodo, declarados en los BIRTH | P0 | ✅ |
| SPB-9 | **Rebirth**: al recibir `NCMD Node Control/Rebirth=true` se reenvía NBIRTH y todos los DBIRTH, y `seq` vuelve a 0 | P0 | ✅ |
| SPB-10 | **DCMD**: escritura de métricas escribibles hacia Modbus. El tag se resuelve por alias o por nombre, se convierte el tipo y la escala, y tras escribir se publica el valor confirmado. Los comandos sobre métricas inexistentes o de solo lectura se descartan y quedan en el log | P0 | ✅ |
| SPB-11 | **NCMD** sobre las salidas locales `DO/n` y sobre `Node Control/Reboot` | P0 | ✅ |
| SPB-12 | Banda muerta (*deadband*) absoluta por tag para los valores analógicos | P1 | ✅ |
| SPB-13 | **Primary Host Application**: si hay un `primary_host_id` configurado, el nodo se suscribe a `spBv1.0/STATE/{id}`, no publica NBIRTH hasta recibir `online:true` y se desconecta, con NDEATH, si el host pasa a `online:false` | P1 | |
| SPB-14 | **Store & forward**: con el broker o el host primario caído, guarda los cambios en un buffer circular en PSRAM, con opción de volcarlo a microSD. Al reconectar los publica con `is_historical=true` | P1 | |
| SPB-15 | `Node Control/Scan Rate` escribible, que permite cambiar el periodo de encuesta global | P2 | |
| SPB-16 | Propiedades de métrica: `engUnit`, `engLow`, `engHigh` y calidad (`Quality`: 0 = BAD, 192 = GOOD) | P1 | |
| SPB-17 | Métricas de diagnóstico del nodo: `Node Info/Uptime`, `Free Heap`, contadores Modbus (OK, timeout, CRC, excepción) y RSSI de Wi-Fi | P1 | |
| SPB-18 | Plantillas (Template) Sparkplug para describir tipos de dispositivo | P2 | |
| SPB-19 | Payload `DataSet` y tipos `Bytes` y `File` | P2 | |

### 4.5 Modbus RTU maestro sobre RS485 (MB)

| ID | Requisito | Prio | MVP |
|---|---|---|---|
| MB-1 | UART configurable: baudrate de 1200 a 115200, paridad N/E/O, 1 o 2 bits de stop y 8 bits de datos | P0 | ✅ |
| MB-2 | Control de dirección RS485 en half-duplex con el pin RTS (GPIO 21). Se prefiere el modo RS485 por hardware de la UART del ESP32-S3; el GPIO por software es la alternativa | P0 | ✅ |
| MB-3 | Funciones de lectura FC01 (coils), FC02 (discrete inputs), FC03 (holding registers) y FC04 (input registers) | P0 | ✅ |
| MB-4 | Funciones de escritura FC05, FC06, FC15 y FC16 (siempre FC16 para valores de 32 bits) | P0 | ✅ |
| MB-5 | Tipos de dato `bool`, `uint16`, `int16`, `uint32`, `int32` y `float32`, con orden de palabras configurable (`big` = ABCD, `little` = CDAB) | P0 | ✅ |
| MB-6 | Tipos `uint64`, `int64`, `float64` y `string`, orden de bytes configurable (BADC, DCBA) y bit dentro de un registro (`bit`: 0–15) | P1 | |
| MB-7 | Escala lineal `value = raw * scale + offset`, con la inversa al escribir | P0 | ✅ |
| MB-8 | Timeout de respuesta y reintentos configurables. Se respeta el silencio de 3,5 caracteres entre tramas y un retardo configurable entre transacciones | P0 | ✅ |
| MB-9 | Detección de fuera de línea: tras `offline_after` ciclos fallidos seguidos el dispositivo se marca fuera de línea (DDEATH). La primera respuesta válida lo devuelve a en línea (DBIRTH) | P0 | ✅ |
| MB-10 | Periodo de encuesta por dispositivo | P0 | ✅ |
| MB-11 | **Optimización por bloques**: los tags contiguos o cercanos del mismo área se agrupan en una sola lectura (hasta 125 registros o 2000 bits, con un hueco máximo configurable) | P1 | |
| MB-12 | Periodo de encuesta por tag, con *fast* y *slow* scan | P2 | |
| MB-13 | Manejo de excepciones Modbus (códigos 1–11) con contadores por dispositivo | P1 | |
| MB-14 | Escrituras de DCMD encoladas con prioridad sobre la encuesta | P1 | |
| MB-15 | Gateway o servidor **Modbus TCP** (puerto 502) que expone los esclavos RTU por Ethernet | P2 | |
| MB-16 | Un único dueño del bus RS485: la encuesta y las escrituras pasan serializadas por una sola tarea o cola | P0 | ✅ |

### 4.6 E/S local (IO)

| ID | Requisito | Prio | MVP |
|---|---|---|---|
| IO-1 | 8 DI publicadas como métricas Boolean del nodo `DI/1`…`DI/8`, con debounce configurable (20 ms por defecto) y detección por interrupción o muestreo a 1 kHz o más | P0 | ✅ |
| IO-2 | 8 DO como métricas Boolean escribibles del nodo `DO/1`…`DO/8`, manejadas por el expansor I²C | P0 | ✅ |
| IO-3 | **Estado seguro de las DO** (*failsafe*), configurable por salida (mantener, OFF u ON), al perder MQTT durante más de N s o al reiniciar | P1 | |
| IO-4 | Contadores de pulsos por DI (UInt64), persistidos periódicamente | P1 | |
| IO-5 | LED RGB de estado: sin red, sin broker, operando y error de Modbus, con colores o patrones documentados | P1 | ✅ |
| IO-6 | Buzzer para alarmas configurables y para confirmar el reset de fábrica | P2 | |
| IO-7 | Enlaces locales DI→DO (lógica simple), aunque no haya red | P2 | |

### 4.7 Configuración (CFG)

| ID | Requisito | Prio | MVP |
|---|---|---|---|
| CFG-1 | Configuración en un documento JSON con el esquema común (§6), validado al cargarlo y con errores descriptivos. Si la configuración es inválida no se arranca la encuesta | P0 | ✅ |
| CFG-2 | Configuración por defecto embebida en el firmware y sobrescribible por un archivo en un sistema de archivos interno (LittleFS) | P0 | ✅ |
| CFG-3 | **Interfaz web** (HTTP) con usuario y contraseña para editar la red, MQTT, Sparkplug, RS485 y los dispositivos y tags, y para ver el estado en vivo | P1 | |
| CFG-4 | API REST JSON para leer y escribir la configuración, con importar y exportar | P1 | |
| CFG-5 | Reset de fábrica con el botón BOOT presionado más de 10 s | P1 | |
| CFG-6 | Consola serie (shell) con comandos de estado, configuración y diagnóstico | P1 | |
| CFG-7 | Secretos (contraseñas, claves TLS) en almacenamiento cifrado | P1 | |

### 4.8 Gestión del firmware (FW)

| ID | Requisito | Prio | MVP |
|---|---|---|---|
| FW-1 | **OTA** por HTTPS con particiones A/B, *rollback* automático si el nuevo firmware no confirma su salud y disparo por web o por NCMD | P1 | |
| FW-2 | Firmware firmado y *secure boot* | P1 | |
| FW-3 | Cifrado de flash | P2 | |
| FW-4 | Versión semántica y hash de git visibles en NBIRTH (`Node Info/Firmware`) | P0 | ✅ |

### 4.9 Robustez y observabilidad (REL)

| ID | Requisito | Prio | MVP |
|---|---|---|---|
| REL-1 | Watchdog de tareas en todas las tareas críticas | P0 | ✅ |
| REL-2 | Memoria acotada: dispositivos y tags con límites estáticos y sin alocaciones en el camino crítico de encuesta y publicación | P0 | ✅ |
| REL-3 | Logs por niveles en la UART de consola | P0 | ✅ |
| REL-4 | Syslog remoto o logs por MQTT | P2 | |
| REL-5 | Registro del motivo del último reinicio, publicado en NBIRTH | P1 | |
| REL-6 | Arranque hasta NBIRTH en menos de 10 s con red y broker disponibles | P1 | |

### 4.10 Calidad (QA)

| ID | Requisito | Prio | MVP |
|---|---|---|---|
| QA-1 | Tests unitarios en el host para la codificación y decodificación Sparkplug, CRC y tramas Modbus, la conversión de tipos y escalas, el reporte por excepción y el parseo de configuración | P0 | ✅ |
| QA-2 | El build para ESP32-S3 sale limpio, sin warnings en el código propio, y se reporta el uso de flash y RAM | P0 | ✅ |
| QA-3 | Prueba de punta a punta en el host o en un emulador, contra mosquitto y un esclavo Modbus simulado | P1 | |
| QA-4 | CI que compila y corre los tests | P1 | |

## 5. MVP: resumen

El MVP es un gateway que se pone en marcha editando un JSON, se conecta por Ethernet (DHCP o IP estática) a un broker MQTT sin TLS y publica con Sparkplug B 3.0 correcto, sin host primario y sin store & forward. Esto incluye:

- birth y death del nodo y de los dispositivos, bdSeq persistido, seq y aliases;
- reporte por excepción con deadband;
- rebirth, y escrituras por DCMD y NCMD.

Encuesta N esclavos Modbus RTU por el RS485 integrado, con los tipos de 16 y 32 bits, escala, timeouts, reintentos y detección de fuera de línea. Expone además las 8 DI y las 8 DO, y muestra su estado en el LED RGB.

**IDs del MVP:** NET-1, NET-2, NET-3, TIME-1, MQTT-1, MQTT-2, MQTT-3, MQTT-7, SPB-1 a SPB-12, MB-1 a MB-5, MB-7 a MB-10, MB-16, IO-1, IO-2, IO-5, CFG-1, CFG-2, FW-4, REL-1 a REL-3, QA-1 y QA-2.

## 6. Esquema común de configuración (obligatorio para ambas implementaciones)

```json
{
  "network": { "dhcp": true, "ip": "192.168.1.50", "netmask": "255.255.255.0",
               "gateway": "192.168.1.1", "dns": "192.168.1.1" },
  "ntp":     { "server": "pool.ntp.org" },
  "mqtt":    { "host": "192.168.1.10", "port": 1883, "client_id": "",
               "username": "", "password": "", "keepalive_s": 30 },
  "sparkplug": { "group_id": "Plant1", "edge_node_id": "GW01", "primary_host_id": "" },
  "rs485":   { "baud": 9600, "parity": "none", "stop_bits": 1,
               "response_timeout_ms": 200, "retries": 2, "inter_frame_delay_ms": 5 },
  "local_io": { "di_debounce_ms": 20 },
  "devices": [
    {
      "name": "Meter1", "unit_id": 1, "poll_ms": 1000, "offline_after": 3,
      "tags": [
        { "name": "Voltage", "area": "input",   "address": 0, "type": "float32",
          "word_order": "big", "scale": 1.0, "offset": 0.0, "deadband": 0.1,
          "writable": false, "units": "V" },
        { "name": "Setpoint", "area": "holding", "address": 10, "type": "int16",
          "scale": 0.1, "writable": true },
        { "name": "Run",      "area": "coil",    "address": 0, "type": "bool",
          "writable": true }
      ]
    }
  ]
}
```

**Reglas del esquema:**

- `area` ∈ {`coil`, `discrete`, `holding`, `input`}.
- `type` ∈ {`bool`, `uint16`, `int16`, `uint32`, `int32`, `float32`}; fuera del MVP también `uint64`, `int64`, `float64` y `string` (§6.1, MB-6).
- `bool` solo vale en `coil` y `discrete`; fuera del MVP, también como bit de un registro `holding` o `input` (clave `bit`, §6.1).
- `writable` solo vale en `coil` y `holding`.
- Si `client_id` está vacío se usa `edge_node_id`.

**Límites del MVP:** hasta 16 dispositivos, 64 tags por dispositivo y 256 tags en total.

**Mapeo de tipos a Sparkplug:**

| Tipo de tag | Datatype Sparkplug |
|---|---|
| `bool` | Boolean (11) |
| `int16` | Int16 (2) |
| `uint16` | UInt16 (6) |
| `int32` | Int32 (3) |
| `uint32` | UInt32 (7) |
| `float32` | Float (9) |
| Cualquier tipo con `scale` ≠ 1 u `offset` ≠ 0 | Float (9) |
| `uint64` / `int64` (MB-6) | UInt64 (8) / Int64 (4), exactos en `long_value` |
| `float64` (MB-6), o `uint64`/`int64`/`float64` con `scale` ≠ 1 u `offset` ≠ 0 | Double (10) |
| `string` (MB-6) | String (12) |
| `bool` con `bit` (MB-6) | Boolean (11) |
| `bytes` (SPB-19) | Bytes (17) |

### 6.1 Claves opcionales fuera del MVP

Todas son opcionales. Si faltan, el equipo se comporta como el MVP. Las claves desconocidas siguen siendo un error.

**`network.wifi` (NET-4): Wi-Fi STA como alternativa o respaldo de Ethernet.**

```json
"network": { "dhcp": true,
             "wifi": { "enabled": true, "ssid": "Planta", "password": "secreto123",
                       "role": "backup", "failback_s": 30, "dhcp": true } }
```

| Clave | Tipo | Defecto | Regla |
|---|---|---|---|
| `enabled` | bool | `false` | `false` = solo Ethernet |
| `ssid` | texto | `""` | 1–32 bytes; obligatorio con `enabled=true` |
| `password` | texto | `""` | vacío (red abierta), 8–63 caracteres (WPA2) o 64 dígitos hexadecimales (PSK) |
| `role` | `backup` \| `primary` | `backup` | `backup`: Ethernet gana la ruta por defecto y Wi-Fi solo se usa sin Ethernet. `primary`: al revés |
| `failback_s` | entero | `30` | 0–3600. Tiempo que la interfaz preferida debe estar arriba, sin cortes, para recuperar la sesión |
| `dhcp`, `ip`, `netmask`, `gateway`, `dns` | como `network` | DHCP | IPv4 de la interfaz Wi-Fi; con `dhcp=false`, `ip`, `netmask` y `gateway` son obligatorias |

Las claves de IPv4 de `network` siguen siendo las de Ethernet.

**Alcance del broker (issue #57).** Una interfaz con enlace y dirección puede no llegar al broker (router o switch aguas arriba caído). Si fallan todos los brokers de la lista (MQTT-6) por la interfaz activa, esta queda "sin servicio": la sesión pasa en el acto a la otra, si es utilizable, sea cual sea su rango. La espera empieza en 60 s y se duplica en cada nuevo fallo seguido, hasta 30 min (`CONFIG_GW_NET_NO_SERVICE_MIN_S` / `_MAX_S`); una sesión estable por esa interfaz la vuelve al mínimo. Al vencer, la preferida recupera la sesión como en el *failback* normal: ese es el reintento. Una interfaz sin servicio se sigue usando si no hay otra.

**`network.hostname` y `network.mdns` (NET-6): nombre del equipo.**

```json
"network": { "dhcp": true, "hostname": "planta1-gw01", "mdns": true }
```

| Clave | Tipo | Defecto | Regla |
|---|---|---|---|
| `hostname` | texto | `""` | Una etiqueta DNS: 1–63 letras, dígitos y `-` (no al principio ni al final), sin puntos. Vacío = derivado de `edge_node_id` (minúsculas; lo que no es letra ni dígito pasa a `-`; `GW01` → `gw01`) |
| `mdns` | bool | `true` | Responde `<hostname>.local` por mDNS y anuncia la web como servicio `_https._tcp` en `web.https_port` (`_http._tcp` en `web.port` con `web.https: false`; instancia `<group_id>/<edge_node_id>`) |

El nombre va también en las peticiones DHCP de Ethernet y Wi-Fi.

**`network.ipv6` (NET-7): IPv6 además de IPv4.**

```json
"network": { "dhcp": true,
             "ipv6": { "enabled": true, "slaac": true, "address": "fd00:1::50", "dns": "" } }
```

| Clave | Tipo | Defecto | Regla |
|---|---|---|---|
| `enabled` | bool | `false` | `false` = solo IPv4. Con `true`, cada interfaz tiene dirección link-local |
| `slaac` | bool | `true` | Autoconfiguración sin estado con los anuncios del router (RA) |
| `address` | texto | `""` | Dirección estática de Ethernet, en forma de texto RFC 4291. Prefijo /64 en el enlace. No se admiten multicast, link-local, `::` ni `::1` |
| `dns` | texto | `""` | Servidor DNS IPv6 (tercera posición; DHCPv4 llena las dos primeras) |

El router por defecto IPv6 se aprende de los RA; no hay *gateway* IPv6 estático. `mqtt.host`, `ntp.server` y `syslog.host` aceptan literales IPv6 (`fd00::10`) o nombres con registro AAAA.

**`mqtt.tls` (MQTT-4, MQTT-5): TLS hacia el broker, opcionalmente mutuo.**

```json
"mqtt": { "host": "broker.planta.local", "port": 8883,
          "tls": { "enabled": true, "ca_file": "ca.pem", "server_name": "",
                   "cert_file": "gw01.crt", "key_file": "gw01.key" } }
```

| Clave | Tipo | Defecto | Regla |
|---|---|---|---|
| `enabled` | bool | `false` | `false` = TCP sin cifrar, como en el MVP. Con `true`, el defecto de `mqtt.port` pasa a `8883` |
| `ca_file` | texto | `""` | Archivo PEM con la o las CA que firman el certificado del broker, en la partición LittleFS junto a `config.json` (`/cfg/<nombre>`). Vacío = el paquete de CA raíz públicas de ESP-IDF. Nombre de 1–32 caracteres `A-Z a-z 0-9 . _ -`, sin `/`, que no empiece con `.` ni sea `config.json` |
| `server_name` | texto | `""` | Nombre que debe llevar el certificado del broker (SAN o CN). Vacío = el `host` al que se conecta (también un literal IP, contra un SAN de tipo IP). La verificación del nombre no se puede desactivar |
| `cert_file` | texto | `""` | MQTT-5: certificado PEM del cliente (TLS mutuo), en LittleFS como `ca_file`. Vacío = sin certificado de cliente |
| `key_file` | texto | `""` | Su clave privada PEM, **sin cifrar** (PKCS#1, SEC1 o PKCS#8). Obligatoria con `cert_file` y prohibida sin él |

TLS 1.2 o 1.3, lo que negocie el broker. Las fechas de validez del certificado se comprueban: la conexión TLS espera a que SNTP fije el reloj. Las claves se validan aunque `enabled` sea `false`.

**`mqtt.protocol` (MQTT-8): versión del protocolo.**

| Clave | Tipo | Defecto | Regla |
|---|---|---|---|
| `protocol` | `"3.1.1"` \| `"5"` | `"3.1.1"` | Versión de MQTT para todos los brokers de la lista. Con `"5"`, como pide Sparkplug 3.0: *Clean Start* = 1 y *Session Expiry Interval* = 0 (el broker no guarda la sesión entre conexiones, igual que *clean session* en 3.1.1) y *Will Delay Interval* = 0 (el NDEATH se publica en cuanto se pierde la conexión). Los temas, QoS, retención y payloads no cambian |

**`mqtt.backup_brokers`, `failover_after`, `primary_return_s` (MQTT-6): lista de brokers.**

```json
"mqtt": { "host": "broker1.planta.local", "port": 1883,
          "backup_brokers": [ { "host": "broker2.planta.local" },
                              { "host": "10.0.0.20", "port": 1884 } ],
          "failover_after": 3, "primary_return_s": 0 }
```

| Clave | Tipo | Defecto | Regla |
|---|---|---|---|
| `backup_brokers` | array | `[]` | Hasta 3 brokers de respaldo, en orden, después de `mqtt.host` (el primario). Cada uno: `host` (obligatorio, no vacío) y `port` (defecto: `mqtt.port`). Credenciales, `client_id`, keepalive y `tls` son los mismos para todos; con TLS el nombre se verifica contra el `host` de cada uno (o `tls.server_name`) |
| `failover_after` | entero | `3` | 1–20. Intentos fallidos seguidos en un broker antes de pasar al siguiente (tras el último, el primero) |
| `primary_return_s` | entero | `0` | 0–86400. `0` = un respaldo que funciona se conserva hasta que falle. Con N > 0, tras N s de sesión estable en un respaldo se cierra en orden (NDEATH) y se prueba **una vez** el primario; si falla, se sigue por el primer respaldo |

Un intento falla si no termina en una sesión estable: conexión TCP o TLS fallida, CONNACK rechazado, SUBSCRIBE rechazado o sin respuesta, o sesión que cae antes de durar un keepalive (como mucho 60 s) desde el NBIRTH. Una sesión estable vuelve a contar desde cero. Cada broker nuevo empieza con el *backoff* mínimo. Con dos interfaces (NET-4), si fallan todos los brokers de la lista (`failover_after` veces cada uno) por la interfaz activa, esa interfaz queda "sin servicio" y la sesión pasa a la otra (ver `network.wifi`).

**`sparkplug.primary_host_id` (SPB-13): Primary Host Application.** Clave del MVP; vacía = sin host primario. Con un id, el nodo se suscribe a `spBv1.0/STATE/{id}` en el mismo SUBSCRIBE que NCMD y DCMD y espera un STATE de Sparkplug 3.0 (JSON `{"online": true|false, "timestamp": <ms>}`, retenido) antes del NBIRTH:

- `online: true` → NBIRTH y DBIRTH;
- `online: false` con la sesión arriba → NDEATH explícito (bdSeq de la sesión) y DISCONNECT; la sesión siguiente vuelve a esperar;
- un STATE con `timestamp` menor que el último aceptado se ignora (mensaje retenido viejo); el último `timestamp` se recuerda entre sesiones, no entre arranques;
- un payload que no es ese JSON (por ejemplo, el `ONLINE` de Sparkplug 2.x) se ignora con un aviso.

Con brokers de respaldo (MQTT-6), un host fuera de línea en un broker (STATE `false`, o ningún STATE `true` en 30 s) cuenta como intento fallido en ese broker. Con un solo broker el nodo queda conectado esperando.

**`sparkplug.store_forward` (SPB-14): store & forward.**

| Clave | Tipo | Defecto | Regla |
|---|---|---|---|
| `store_forward` | bool u objeto | `false` | `false` = sin sesión Sparkplug los cambios se coalescen y la sesión siguiente publica solo el valor actual (como el MVP). `true` = los cambios que se habrían publicado (mismas reglas de excepción y *deadband*) se guardan en un búfer circular y, tras el NBIRTH y los DBIRTH de la sesión siguiente, se publican del más viejo al más nuevo como DDATA/NDATA con `is_historical = true` y el timestamp de la muestra |

"Sin sesión" incluye broker caído y host primario (SPB-13) fuera de línea, desde el primer NBIRTH del arranque (antes no hay estado publicado con qué comparar). El búfer tiene 40 000 muestras en PSRAM (960 KB) o 512 en RAM interna si no hay PSRAM (Kconfig `GW_SF_PSRAM_SAMPLES` / `GW_SF_INTERNAL_SAMPLES`); lleno, se pisan las más viejas. Las muestras de un dispositivo sin DBIRTH en la sesión nueva (fuera de línea al volver) se descartan. Los enteros de 64 bits y los textos (MB-6) se guardan exactos.

Forma de objeto, con el volcado opcional a microSD:

```json
"sparkplug": { "group_id": "Plant1", "edge_node_id": "GW01",
               "store_forward": { "enabled": true, "sd": { "enabled": false, "max_kb": 16384 } } }
```

| Clave | Tipo | Defecto | Regla |
|---|---|---|---|
| `enabled` | bool | `true` | Lo mismo que `store_forward: true` |
| `sd.enabled` | bool | `false` | Con el búfer de RAM lleno, sus muestras más viejas pasan a la tarjeta microSD (de a bloques) en lugar de pisarse; al volver la sesión se publican primero las de la tarjeta. Requiere `enabled`. **GPIO45 (D0 de la tarjeta) es pin de *strapping*: ver la advertencia de `idf/README.md` §6.5 antes de activarlo** |
| `sd.max_kb` | entero | `16384` | 64–4 194 304. Espacio máximo en la tarjeta; lleno, se borra el segmento más viejo (contado como perdido) |

En la tarjeta: carpeta `/sf` de una tarjeta FAT (no se formatea nunca), segmentos numerados de solo escritura al final, que se borran enteros al publicarse. Sobreviven a un reinicio y se publican después si la lista de dispositivos y tags no cambió; si cambió, se borran.

**Tipos de 64 bits, `string`, órdenes de bytes y bits de registro (MB-6), en cada tag.**

```json
{ "name": "Energy",  "area": "holding", "address": 30, "type": "uint64" },
{ "name": "Counter", "area": "holding", "address": 34, "type": "int64", "word_order": "CDAB", "writable": true },
{ "name": "Flow",    "area": "input",   "address": 40, "type": "float64", "word_order": "DCBA" },
{ "name": "Serial",  "area": "holding", "address": 50, "type": "string", "length": 8 },
{ "name": "Alarm",   "area": "holding", "address": 60, "type": "bool", "bit": 3, "writable": true }
```

| Clave | Tipo | Defecto | Regla |
|---|---|---|---|
| `type` | + `uint64` \| `int64` \| `float64` \| `string` | — | 64 bits = 4 registros; `string` = `length` registros |
| `word_order` | `big` \| `little` \| `ABCD` \| `CDAB` \| `BADC` \| `DCBA` | `big` | Letras = bytes del valor, del más significativo (A) al menos, en el orden en que llegan. `big` = `ABCD`, `little` = `CDAB` (palabra menos significativa primero), `BADC` = bytes invertidos en cada registro, `DCBA` = todo invertido. Vale igual para 32 y 64 bits; en 16 bits solo cuenta la inversión de bytes |
| `length` | entero | — | 1–16 registros; obligatorio con `type: string` y solo con él. Dos caracteres ASCII por registro, el primero en el byte alto (en el bajo con `BADC`/`DCBA`; el orden de palabras no se aplica) |
| `bit` | entero | — | 0–15, con `type: bool` en `holding` o `input` (obligatorio ahí). 0 = bit menos significativo del registro tal como llega; no admite `word_order` distinto de `big` |

- **Exactitud.** `int64` y `uint64` sin escala se publican exactos (UInt64/Int64), también por encima de 2^53; con `scale`/`offset` pasan a Double.
- **`string`:** se corta en el primer NUL y se quitan los espacios finales; lo que no es ASCII imprimible se publica como `?`. Al escribir se rellena con NUL; un texto más largo que `2 × length` se rechaza. No admite `scale`, `offset` ni `deadband`.
- **Escritura de un bit:** lectura del registro (FC03) y escritura del registro completo con el bit cambiado (FC06, o FC16 con escrituras múltiples forzadas).

**Bloque de registros crudo (SPB-19), `type: bytes`.**

```json
{ "name": "Block", "area": "holding", "address": 220, "type": "bytes", "length": 3, "writable": true }
```

- `length` registros (1–16, obligatorio) que se publican como métrica **Bytes** (17): dos bytes por registro, el alto primero (el bajo primero con `BADC`/`DCBA`; el orden de palabras no se aplica). Sin ningún otro proceso: un NUL o un byte no ASCII viajan tal cual.
- No admite `scale`, `offset`, `deadband`, `eng_low` ni `eng_high`. El reporte por excepción compara todos los bytes.
- Un DCMD debe traer un valor Bytes de exactamente `2 × length` bytes; se escribe con una FC16.

**Propiedades de métrica (SPB-16), en cada tag.**

```json
{ "name": "Voltage", "area": "input", "address": 0, "type": "float32", "units": "V",
  "eng_low": 0, "eng_high": 400 }
```

| Clave | Tipo | Defecto | Regla |
|---|---|---|---|
| `units` | texto | `""` | Hasta 15 caracteres. Se publica como la propiedad `engUnit` (String) en el DBIRTH; vacío = sin propiedad |
| `eng_low` | número | — | Propiedad `engLow` (Double) en el DBIRTH. No vale en tags `bool` ni `string` |
| `eng_high` | número | — | Propiedad `engHigh` (Double) en el DBIRTH. Con las dos claves, `eng_high` > `eng_low` |

- **Calidad (`Quality`, Int32):** 192 (GOOD) cuando la lectura respondió, 0 (BAD) cuando falló (excepción Modbus) o el tag nunca se leyó; con BAD el valor va además como `is_null`. Todo DBIRTH la lleva en cada métrica; un DDATA, solo cuando cambia. Las DO del nodo usan la misma regla: BAD mientras el expansor no responde.
- Las propiedades viajan en el `PropertySet` de la métrica (claves y valores en el mismo orden). Un comando (NCMD/DCMD) con propiedades se acepta; sus propiedades se ignoran.

**Lecturas en bloque por dispositivo (MB-11), en cada dispositivo.**

```json
{ "name": "Meter1", "unit_id": 1, "max_gap": 10, "tags": [ … ] },
{ "name": "Picky",  "unit_id": 7, "block_reads": false, "tags": [ … ] }
```

| Clave | Tipo | Defecto | Regla |
|---|---|---|---|
| `block_reads` | bool | el del firmware (`true`) | `false` = una petición por tag, para esclavos que responden excepción si un bloque incluye registros no mapeados |
| `max_gap` | entero | el del firmware (`0`) | 0–1999. Registros o bits sin usar que un bloque puede saltar para unir dos tags del mismo área. No se admite con `block_reads: false`. Un bloque nunca pasa de 125 registros ni de 2000 bits |

Sin estas claves vale la opción de compilación (`CONFIG_GW_MB_BLOCK_READS`, `CONFIG_GW_MB_BLOCK_MAX_GAP`), como en el MVP.

**Tipos de dispositivo como plantillas Sparkplug (SPB-18), en cada dispositivo.**

```json
{ "name": "PumpA", "unit_id": 1, "type": "Pump", "tags": [ … ] },
{ "name": "PumpB", "unit_id": 2, "type": "Pump", "tags": [ … ] }
```

| Clave | Tipo | Defecto | Regla |
|---|---|---|---|
| `type` | texto | `""` | Hasta 39 caracteres, sin `/ + #` ni `bdSeq`. Vacío = sin plantilla (una métrica por tag, como el MVP) |

- Todos los dispositivos de un mismo `type` tienen los mismos tags: mismos nombres y datatypes, en el mismo orden (área, dirección, escala y unidades pueden cambiar). Si no, la configuración se rechaza con la ruta `devices[i].type`.
- Las definiciones juntas describen como mucho **64 tags** (cada tipo cuenta una vez): el NBIRTH las lleva todas.
- **NBIRTH:** una métrica Template (19) por tipo, con su nombre, `is_definition = true`, sin alias ni `template_ref`. Sus miembros: nombre, datatype, un valor por defecto (0, falso o texto vacío) y las propiedades `engUnit`/`engLow`/`engHigh` del primer dispositivo del tipo.
- **DBIRTH:** una sola métrica Template, la instancia: nombre = el tipo, alias 100 + el primer tag del dispositivo, `template_ref` = el tipo. Sus miembros son los tags, por nombre y sin alias, con valor y `Quality`.
- **DDATA:** la instancia por alias, solo con los miembros que cambiaron (por nombre).
- **DCMD:** la instancia con los miembros a escribir (por nombre); también se acepta el tag por nombre como métrica suelta. Por alias, no: los miembros no tienen alias.

**Encuesta rápida y lenta (MB-12).**

```json
{ "name": "Meter1", "unit_id": 1, "poll_ms": 1000, "slow_poll_ms": 30000, "tags": [
    { "name": "Voltage", "area": "input",   "address": 0,  "type": "float32" },
    { "name": "Serial",  "area": "holding", "address": 50, "type": "string", "length": 8, "scan": "slow" } ] }
```

| Clave | Dónde | Tipo | Defecto | Regla |
|---|---|---|---|---|
| `scan` | tag | `fast` \| `slow` | `fast` | `fast` se lee cada `poll_ms`; `slow`, cada `slow_poll_ms` |
| `slow_poll_ms` | dispositivo | entero | 10 × `poll_ms` (tope 3 600 000) | 50–3 600 000 y nunca menor que `poll_ms` |

- Los tags rápidos y los lentos nunca comparten un bloque de lectura.
- **Fuera de línea (MB-9):** un ciclo que solo lee los tags rápidos cuenta como cualquier otro: falla si una petición queda sin respuesta. Pero un dispositivo pasa (o vuelve) a en línea solo con un ciclo que leyó **todos** sus bloques, así que su DBIRTH lleva todos los tags. Mientras está fuera de línea, cada ciclo incluye los lentos.
- Si un ciclo falla antes de leer los lentos, quedan pendientes para el ciclo siguiente.
- Un dispositivo con solo tags lentos se encuesta cada `slow_poll_ms`.

**`modbus_tcp` (MB-15): gateway Modbus TCP hacia los esclavos RTU.**

```json
"modbus_tcp": { "enabled": true, "port": 502, "allowed_clients": ["192.168.1.20", "10.0.5.0/24"],
                "idle_timeout_s": 60 }
```

| Clave | Tipo | Defecto | Regla |
|---|---|---|---|
| `enabled` | bool | `false` | Modbus TCP **no tiene autenticación**: el servidor solo arranca si se pide |
| `port` | entero | `502` | 1–65535 |
| `allowed_clients` | array de textos | `[]` | Hasta 8 direcciones IPv4 (`a.b.c.d`) o redes (`a.b.c.d/n`, n 0–32). Una conexión de otro origen se cierra sin respuesta. Vacío = cualquier cliente (el equipo lo advierte en el log) |
| `idle_timeout_s` | entero | `60` | 0–3600. Una conexión sin peticiones en ese tiempo se cierra; 0 = nunca |

- Se reenvían FC01–FC06, FC15 y FC16 a las unidades 1–247, tengan o no un dispositivo configurado. El gateway responde él mismo, con excepción, a cualquier otra función (01), a datos no válidos (03) y a las unidades 0 y 248–255 (0A). Si la unidad no responde tras los reintentos de `rs485`, la excepción es 0B.
- **Arbitraje del bus (MB-16):** las peticiones TCP pasan por la tarea dueña del bus, una a la vez y con la misma granularidad que los DCMD: entre dos peticiones de la encuesta. Prioridad: escrituras DCMD, después Modbus TCP, después la encuesta.

**`local_io`: E/S local sin red. Estado seguro de las DO (IO-3).** Todas las claves de `local_io` son opcionales (IO-3, IO-4, IO-6 e IO-7).

```json
"local_io": { "di_debounce_ms": 20, "failsafe_s": 30,
              "do_failsafe": ["off", "on", "hold", "off", "off", "off", "off", "off"] }
```

| Clave | Tipo | Defecto | Regla |
|---|---|---|---|
| `failsafe_s` | entero | `0` | 0–86400. Segundos sin sesión Sparkplug (sin un NBIRTH vigente) tras los que cada DO toma su estado seguro. 0 = la pérdida de MQTT no cambia las DO |
| `do_failsafe` | array de textos | todas `off` | Hasta 8 entradas, `DO/1` primero; cada una `off`, `on` u `hold`. Las que faltan son `off` |

- **Al reiniciar** (encendido, reinicio por software, *panic* o watchdog): `off` y `on` toman ese estado en cuanto la configuración es válida; `hold` conserva el estado que el expansor TCA9554 mantuvo durante el reinicio. Tras un corte de alimentación el expansor también se apaga: `hold` arranca en OFF.
- **Antes de cargar la configuración** todas las DO pasan a OFF, salvo las `hold` de la última configuración válida (guardadas en NVS), que no se tocan. Las `on` se encienden recién con la configuración validada.
- **Configuración rechazada** (recuperación) o puesta en marcha: todas a OFF, también las `hold`.
- **Sin sesión durante `failsafe_s`** (contados desde el arranque o desde la pérdida): las `off` y las `on` toman ese estado, una vez; las `hold` no cambian. Al volver la sesión, las DO quedan como están hasta el próximo NCMD; el NBIRTH informa su estado real.
- Sin estas claves el equipo se comporta como el MVP: todas a OFF al reiniciar y sin reacción a la pérdida de MQTT.

**Contadores de pulsos de las DI (IO-4), en `local_io`.**

```json
"local_io": { "di_debounce_ms": 20, "counter_save_s": 600 }
```

| Clave | Tipo | Defecto | Regla |
|---|---|---|---|
| `counter_save_s` | entero | `600` | 0 o 60–86400. Cada cuánto se guardan los contadores en NVS, solo si cambiaron. 0 = no se guardan (vuelven a 0 en cada arranque) |

- Cada DI cuenta sus **activaciones** (paso de inactiva a activa, después del debounce): con 20 ms de debounce, hasta unos 25 Hz. Un pulso más corto que el debounce no cuenta.
- Métricas del nodo `Counter/1`…`Counter/8` (UInt64, alias 30–37), en el NBIRTH y por excepción en el mismo NDATA que el cambio de la DI.
- Al arrancar se restauran del último guardado. Un reinicio ordenado (NCMD Reboot, configuración nueva, `restart` del shell o de la web) los guarda antes; un corte de alimentación, un *panic* o el watchdog pierden lo contado desde el último guardado (como mucho `counter_save_s`).
- No se pueden escribir por NCMD. El reset de fábrica los pone a 0.

**Enlaces locales DI→DO (IO-7), en `local_io.rules`.**

```json
"local_io": { "rules": [ { "di": 2, "do": 5, "mode": "follow" },
                         { "di": 3, "do": 6, "mode": "toggle" } ] }
```

| Clave | Tipo | Defecto | Regla |
|---|---|---|---|
| `rules` | array de objetos | `[]` | Hasta 8 reglas; cada DO en una sola regla |
| `di` | entero | — | 1–8, obligatorio |
| `do` | entero | — | 1–8, obligatorio |
| `mode` | `follow` \| `invert` \| `toggle` | `follow` | `follow`: DO = DI. `invert`: DO = no DI. `toggle`: cada activación de la DI (flanco a activa, después del debounce) invierte la DO |

- Las reglas se evalúan en el equipo, con o sin red ni broker: un cambio de DI llega a la DO en pocos milisegundos más el debounce.
- **Prioridades:** una DO con regla es de la regla. Un NCMD sobre ella se rechaza (queda en el log y el NDATA confirma el estado real) y el estado seguro por pérdida de MQTT (IO-3) no la toca. Al reiniciar arranca como cualquier DO (`off` u `hold` en `do_failsafe`; `on` se rechaza para una DO con regla) hasta que la regla se evalúa con las DI del arranque: `follow` e `invert` toman enseguida el valor de su DI; `toggle` sigue desde ese estado de arranque.
- Las DO sin regla se manejan como siempre: NCMD con sesión, estado seguro sin ella.

**Buzzer (IO-6), en `local_io.buzzer`.**

```json
"local_io": { "buzzer": { "alarms": ["config_error", "mqtt_down", "device_offline"],
                          "alarm_after_s": 30 } }
```

| Clave | Tipo | Defecto | Regla |
|---|---|---|---|
| `alarms` | array de textos | `[]` | Alarmas que suenan: `config_error` (configuración rechazada), `mqtt_down` (sin sesión Sparkplug), `device_offline` (algún dispositivo Modbus fuera de línea). Sin repetidos. Vacío = el buzzer solo confirma gestos del botón BOOT |
| `alarm_after_s` | entero | `30` | 0–3600. Cuánto debe durar `mqtt_down` o `device_offline` antes de sonar. `config_error` suena enseguida |

- **Patrones:** un pitido es 150 ms de tono y 150 ms de silencio. Cada alarma repite su ráfaga cada 10 s mientras dura: `device_offline` 1 pitido, `mqtt_down` 2, `config_error` 3. Si hay varias, suena la de más pitidos.
- **`config_error`:** una configuración rechazada no puede activar nada, así que vale lo que pedía la última configuración válida (guardado en NVS). Tras un reset de fábrica no suena.
- **Botón BOOT:** 1 pitido al soltarlo entre 2 y 10 s (punto de acceso, NET-5) y 2 pitidos al confirmar el reset de fábrica (CFG-5), haya alarmas o no.
- No hay forma de silenciar una alarma mientras dura su condición.

**Diagnóstico del nodo (SPB-17, MB-13), en `sparkplug`.**

```json
"sparkplug": { "group_id": "Plant1", "edge_node_id": "GW01", "diagnostics_s": 60 }
```

| Clave | Tipo | Defecto | Regla |
|---|---|---|---|
| `diagnostics_s` | entero | `60` | 0 o 10–86400. Cada cuánto se publica un NDATA con el diagnóstico. 0 = solo en el NBIRTH |

Métricas del nodo, en el NBIRTH y en ese NDATA periódico:

| Métrica | Alias | Datatype | Qué es |
|---|---|---|---|
| `Node Info/Uptime` | 50 | UInt64 | Segundos desde el arranque |
| `Node Info/Free Heap` | 51 | UInt32 | Bytes libres de RAM interna |
| `Node Info/Min Free Heap` | 52 | UInt32 | Mínimo de RAM interna libre desde el arranque |
| `Node Info/RSSI` | 53 | Int32 | dBm del punto de acceso Wi-Fi; solo si el equipo tiene Wi-Fi configurado, `null` mientras no está asociado |
| `Node Info/Modbus` | 54 | DataSet | Una fila por dispositivo: `Device` (String), `Online` (Boolean), `Cycles`, `Failed Cycles`, `OK`, `Timeout`, `CRC`, `Frame`, `Exception` (UInt32; los tres últimos cuentan intentos, reintentos incluidos) |

- La tabla Modbus va en el NDATA solo si algún contador o estado cambió desde la última vez; uptime y heap van siempre.
- Los contadores son del nodo, no de cada dispositivo: así se publican también los de un dispositivo fuera de línea (sin DBIRTH), que es cuando más crecen.

**`syslog` (REL-4): copia del log a un servidor syslog.**

```json
"syslog": { "host": "192.168.1.20", "port": 514, "level": "info" }
```

| Clave | Tipo | Defecto | Regla |
|---|---|---|---|
| `host` | texto | `""` | Nombre, IPv4 o IPv6 del servidor. Vacío = sin syslog remoto |
| `port` | entero | `514` | 1–65535, UDP |
| `level` | `error` \| `warning` \| `info` \| `debug` | `info` | Severidad mínima que se envía |

Formato: RFC 5424 sobre UDP (RFC 5426), un mensaje por datagrama, *facility* `local0`, HOSTNAME = `edge_node_id`, APP-NAME = módulo del log y `[meta sequenceId="n"]`. Ejemplo: `<134>1 2026-11-04T12:00:00.123Z GW01 edge - - [meta sequenceId="12"] Sparkplug session up (bdSeq=3)`.

**`web` (CFG-3, CFG-4): interfaz web y API REST.**

```json
"web": { "enabled": true, "port": 80, "username": "admin", "password": "una-clave-larga" }
```

| Clave | Tipo | Defecto | Regla |
|---|---|---|---|
| `enabled` | bool | `true` | `false` = sin servidor web con la configuración válida. Con la configuración rechazada o en puesta en marcha el servidor arranca igual (recuperación) |
| `port` | entero | `80` | 1–65535, HTTP. Con `https` en `true` solo redirige (301/308) a HTTPS |
| `https` | bool | `true` | HTTPS con el certificado propio del equipo (EC P-256 autofirmado, generado en el primer arranque y guardado con los secretos en NVS; su huella SHA-256 sale en la consola en cada arranque). `false` = HTTP plano como antes |
| `https_port` | entero | `443` | 1–65535, distinto de `port` |
| `username` | texto | `"admin"` | 1–32 caracteres ASCII visibles, sin `:` |
| `password` | texto (secreto) | `""` | Vacío o al menos 8 caracteres. Vacío = el equipo acepta `admin` con su **contraseña de puesta en marcha** (aleatoria, se ve en la consola) |

**Secretos** (`mqtt.password`, `network.wifi.password`, `web.password`): no se guardan en el JSON, sino en NVS (CFG-7); `web.password` solo como hash PBKDF2-HMAC-SHA256 con sal. Al exportar, un secreto definido vale `"********"`. Al importar (PUT):

- `"********"` conserva el secreto guardado;
- otro texto lo reemplaza (`""` lo borra);
- la clave ausente lo borra: el documento reemplaza la configuración entera.

En el archivo `/cfg/config.json` un secreto ausente, vacío o `"********"` toma el valor de NVS; uno en claro (archivo grabado a mano o de un firmware anterior) se usa, se mueve a NVS y el archivo se reescribe sin él.

**Límite de intentos:** tras 5 accesos fallidos seguidos desde una misma dirección, esa dirección recibe `429` con `Retry-After` durante 30 s, sin comprobar la contraseña (tampoco la correcta); cada bloqueo seguido duplica el tiempo, hasta 15 min; un acceso correcto lo borra (Kconfig `GW_WEB_LOGIN_*`).

**API REST** (todas las rutas piden HTTP Basic con `web.username` y `web.password`, por HTTPS con `https`; respuestas JSON):

| Método y ruta | Qué hace |
|---|---|
| `GET /api/config` | Exporta la configuración guardada, con los secretos enmascarados. `?download=1` la entrega como archivo |
| `PUT /api/config` (o `POST`) | Importa: valida, guarda de forma atómica (archivo temporal + `rename`) y reinicia de forma ordenada (NDEATH). `?dry_run=1` solo valida. Error: `400 {"ok":false,"error":"…","path":"devices[0].tags[1].type","line":0,"column":0}`; `413` si supera `CONFIG_GW_CONFIG_MAX_BYTES` |
| `POST /api/restart` | Reinicio ordenado |

## 7. Criterios de aceptación del MVP

1. `NBIRTH` con `seq`=0 y el `bdSeq` del LWT. Al matar la conexión TCP el broker publica `NDEATH` con ese mismo `bdSeq`.
2. Al apagar un esclavo se publica DDEATH en `offline_after × poll_ms` (más el tiempo de los timeouts). Al volver a encenderlo se publica DBIRTH.
3. Un cambio en una DI se ve en NDATA en menos de 100 ms. Un NCMD `DO/3=true` activa la salida 3 y el NDATA lo confirma.
4. Un DCMD `Setpoint=12.3` escribe 123 en el registro holding 10 (FC06) y se confirma por DDATA.
5. Un NCMD `Node Control/Rebirth` reemite NBIRTH y todos los DBIRTH, con `seq` reiniciado.
6. Los payloads se decodifican sin errores con `sparkplug_b.proto` oficial, por ejemplo con Python y `protobuf`.
