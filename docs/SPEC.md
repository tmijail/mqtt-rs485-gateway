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
- `type` ∈ {`bool`, `uint16`, `int16`, `uint32`, `int32`, `float32`}.
- `bool` solo vale en `coil` y `discrete`.
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

**`network.hostname` y `network.mdns` (NET-6): nombre del equipo.**

```json
"network": { "dhcp": true, "hostname": "planta1-gw01", "mdns": true }
```

| Clave | Tipo | Defecto | Regla |
|---|---|---|---|
| `hostname` | texto | `""` | Una etiqueta DNS: 1–63 letras, dígitos y `-` (no al principio ni al final), sin puntos. Vacío = derivado de `edge_node_id` (minúsculas; lo que no es letra ni dígito pasa a `-`; `GW01` → `gw01`) |
| `mdns` | bool | `true` | Responde `<hostname>.local` por mDNS y anuncia la web como servicio `_http._tcp` (instancia `<group_id>/<edge_node_id>`) |

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

Un intento falla si no termina en una sesión estable: conexión TCP o TLS fallida, CONNACK rechazado, SUBSCRIBE rechazado o sin respuesta, o sesión que cae antes de durar un keepalive (como mucho 60 s) desde el NBIRTH. Una sesión estable vuelve a contar desde cero. Cada broker nuevo empieza con el *backoff* mínimo.

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
| `port` | entero | `80` | 1–65535, HTTP |
| `username` | texto | `"admin"` | 1–32 caracteres ASCII visibles, sin `:` |
| `password` | texto (secreto) | `""` | Vacío o al menos 8 caracteres. Vacío = el equipo acepta `admin` con su **contraseña de puesta en marcha** (aleatoria, se ve en la consola) |

**Secretos** (`mqtt.password`, `network.wifi.password`, `web.password`): no se guardan en el JSON, sino en NVS (CFG-7); `web.password` solo como hash PBKDF2-HMAC-SHA256 con sal. Al exportar, un secreto definido vale `"********"`. Al importar (PUT):

- `"********"` conserva el secreto guardado;
- otro texto lo reemplaza (`""` lo borra);
- la clave ausente lo borra: el documento reemplaza la configuración entera.

En el archivo `/cfg/config.json` un secreto ausente, vacío o `"********"` toma el valor de NVS; uno en claro (archivo grabado a mano o de un firmware anterior) se usa, se mueve a NVS y el archivo se reescribe sin él.

**API REST** (todas las rutas piden HTTP Basic con `web.username` y `web.password`; respuestas JSON):

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
