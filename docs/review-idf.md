# Revisión independiente — implementación ESP-IDF del gateway RS485 → Sparkplug B

Fecha: 2026-11-03 · Revisor: agente independiente · Commit revisado: `cb66920` (`idf/`)

Alcance: `idf/` contra `docs/SPEC.md` (§5 IDs del MVP, §6 esquema, §7 criterios de aceptación) y `docs/feasibility-idf.md`. Las afirmaciones de `idf/README.md` se trataron como hipótesis a verificar. No se modificó código.

## 1. Resumen

Es una implementación **sólida y bien diseñada**:

- **Lógica pura separada del HAL.** `gw_core` tiene 2 531 líneas efectivas de C sin dependencias de IDF y se prueba en el host.
- **Dueño único del bus RS485**, en su propia tarea.
- **Memoria propia estática** y colas acotadas.
- **Sparkplug B 3.0 correcto en todo lo que pide el MVP:** bdSeq por intento y persistido, will = NDEATH, seq compartido con vuelta a 0, alias, rebirth, DBIRTH solo tras el NBIRTH, y DCMD por alias o por nombre.

Toda la evidencia declarada **se reprodujo**:

- build esp32s3 sin warnings y con el tamaño idéntico al publicado;
- 54/54 tests de host;
- decodificación con el proto oficial;
- E2E en QEMU 22/22.

Se encontraron **1 defecto alto, 3 medios y varios bajos**:

- **Alto.** Una configuración válida pero mayor de 32 KB, o una LittleFS ilegible, hace que el equipo arranque **en silencio con la configuración de ejemplo embebida**, con otra identidad Sparkplug y otro broker. Esto viola CFG-1.
- **Medio.**
  - Un bloque Modbus que deja de responder, mientras otro del mismo equipo sí responde, deja **valores congelados marcados como válidos** para siempre.
  - **A 1200 baudios toda escritura FC16 falla**: el timeout de TX es fijo de 100 ms. El esclavo igual la ejecuta, hasta 3 veces.
  - El **watchdog puede disparar en un rebirth sobre un enlace lento**: hasta 17 publicaciones bloqueantes sin alimentar el TWDT.

Nada se probó en hardware real, y la documentación lo advierte con honestidad.

## 2. Reproducción de la evidencia

Todo se hizo sobre una copia en el scratchpad: `-B build-rev`, `build-host-rev`, `build-qemu`. Los puertos E2E se cambiaron a 28831, 25020 y 25555 para no chocar con la otra revisión. Se compiló con `cmake --build -j2`.

| Paso | Comando (resumen) | Resultado |
|---|---|---|
| Firmware esp32s3 | `idf.py -B build-rev set-target esp32s3 reconfigure` + `cmake --build build-rev -j2` | **OK, 0 warnings** (1160/1160 objetivos). Binario de 0xAE440 B; queda libre el 83 % de `ota_0` |
| `idf.py size` | — | Flash `.text` 474 192 B; `.rodata` y otros 152 784 B; **DIRAM 150 321 / 341 760 (43,98 %)**: `.bss` 80 048, `.text` 53 895, `.data` 16 378; IRAM 16 384 (100 %). **Imagen total 713 669 B**, idéntica al README |
| Tests de host | `cmake -S idf/test/host …` + `ctest` | **9/9 ejecutables, 54/54 casos OK**. 0 warnings con `-Wall -Wextra -Werror -Wshadow -Wconversion` en `gw_core` |
| Interoperabilidad | `spb_dump` + `spb_decode.py --check` (protobuf 7.36.2) | **5 payloads, 0 errores estructurales** |
| E2E QEMU | build `sdkconfig.qemu`, merge-bin, `e2e.py` | **22/22 PASS** (detalle abajo) |

Detalle del E2E:

- NBIRTH a 2,0 s del arranque.
- DDEATH en 3,04 s (cota 5,8 s); DBIRTH 0,72 s después de volver el esclavo.
- DI → NDATA en 25 ms.
- DCMD Setpoint = 12,3 → `(6, 10, [123])`.
- Backoff 1000 → 1998 → 3749 → 6040 ms.
- bdSeq persistido y NDEATH con el mismo bdSeq tras SIGKILL.
- NCMD Reboot con un único NDEATH.
- 29 mensajes decodificados.

Matices de la reproducción:

1. **SNTP del harness.** El respondedor SNTP no pudo usar el UDP 123: *Address already in use*, ocupado por otro proceso del host. El check TIME-1 pasó porque respondió un servidor NTP ajeno en ese puerto. Es la misma fragilidad que el README ya admite.
2. **Versión de firmware.** La copia sin `.git` compila con `PROJECT_VER = 0.1.0+unknown`. El mecanismo FW-4 depende de compilar dentro del repo.
3. **Prueba propia del revisor.** Se agregó en el scratchpad (no en el repo) `probe_partial.c`, enlazado contra `libgw_core.a`, para confirmar el hallazgo M1. Resultado: tras 10 ciclos con FC04 sin respuesta, `Voltage` sigue `valid=1 val=230.5` con el timestamp del primer ciclo y el dispositivo sigue en línea (30 timeouts contabilizados).

## 3. Auditoría de IDs del MVP

Leyenda: ✅ correcto · ⚠️ parcial o con defecto · ❌ ausente. Las rutas son relativas a `idf/components/` salvo indicación.

| ID | Nota | Referencia | Comentario |
|---|---|---|---|
| NET-1 | ✅ | `gw_net/gw_net.c:98-138`, `:34-45` | W5500 por SPI2 con **INT** (GPIO12, `poll_period_ms = 0`), RST 39, MAC del eFuse. Enlace caído → `ip_changed(false)`. Solo compilado |
| NET-2 | ✅ | `gw_net.c:71-96` | IP estática: `dhcpc_stop` + `set_ip_info` + DNS. Se verificó en `esp_netif_handlers.c:35-78` que `esp_netif_action_connected` publica `GOT_IP` para IP estática al subir el enlace, así que el flujo cierra |
| NET-3 | ✅ | `gw_edge/gw_edge.c:395` | El hostname va a esp-mqtt, que resuelve en cada intento |
| TIME-1 | ✅ | `gw_net.c:156-159`, `:61-63`; `gw_core/port/gw_port_esp.c:237-242` | SNTP con servidor configurable y ms UTC. Si sincroniza después del NBIRTH se hace rebirth (`gw_edge.c:319-324`) |
| MQTT-1 | ✅ | `gw_edge.c:393-415` | 3.1.1, host, puerto, client_id (por defecto `edge_node_id`, `gw_config.c:524-526`), usuario, contraseña y keepalive |
| MQTT-2 | ✅ | `gw_core/src/gw_backoff.c`, `gw_edge.c:247-264` | Auto-reconexión desactivada; exponencial 1–60 s con jitter de hasta −25 % |
| MQTT-3 | ✅ | `gw_edge.c:224-232`, `:407` | El will se regenera con el bdSeq de cada intento (`esp_mqtt_set_config`) |
| MQTT-7 | ✅ | `gw_edge.c:53-62`, `:413-414`; `sdkconfig.defaults` | Colas y pool estáticos, `outbox.limit` 8 KB, `SKIP_PUBLISH_IF_DISCONNECTED`. QoS 0 no pasa por el outbox. Ver M3 (bloqueo frente a TWDT) |
| SPB-1 | ✅ | `gw_core/src/spb.c`, `proto/sparkplug_b.options` | El proto es **idéntico byte a byte al de eclipse/tahu `master`** (verificado con diff). nanopb 0.4.9.1 con callbacks para `metrics` |
| SPB-2 | ✅ | `gw_edge.c:407`; `gw_node.c:375-383`; `gw_edge.c:212-220` | Will QoS 1 sin retain, `bdSeq` UInt64. Desconexión ordenada (Reboot): NDEATH QoS 1 explícito y luego `esp_mqtt_client_stop()`, que envía DISCONNECT (un solo NDEATH en el E2E) |
| SPB-3 | ✅ | `gw_storage/gw_storage.c:46-63`; `gw_edge.c:226` | `uint8_t` que vuelve de 255 a 0, persistido en NVS **antes** de usarse, y uno nuevo por intento. El NBIRTH usa `s_bdseq` del mismo `prepare_attempt` que armó el will |
| SPB-4 | ✅ | `gw_core/src/gw_node.c:101-125` | seq = 0 (`:145`), QoS 0 sin retain, timestamp. bdSeq, Rebirth, Reboot, Firmware, Hardware, DI/1-8 y DO/1-8, todas con nombre, alias, datatype y valor |
| SPB-5 | ✅ | `gw_node.c:39-42`, `:48-50`, `:96` | `uint8_t` compartido por NBIRTH, DBIRTH, DDEATH, NDATA y DDATA; NDEATH con `has_seq = false`. El seq se consume aunque falle el publish, lo que es aceptable |
| SPB-6 | ✅ | `gw_node.c:127-141`, `:172-188` | DBIRTH solo con `n->online` (después del NBIRTH) y con valores actuales; DDEATH con seq al pasar a fuera de línea. Ver M1 |
| SPB-7 | ✅ | `gw_node.c:189-201`, `:204-228` | Un DDATA por ciclo, solo alias, timestamp por métrica |
| SPB-8 | ✅ | `gw_core/include/gw_node.h:30-37` | 1–5 nodo, 10–17 DI, 20–27 DO, 100 + n tags. Únicos por nodo, UInt64 |
| SPB-9 | ✅ | `gw_node.c:143-155`, `:371-374` | NBIRTH con seq 0 y mismo bdSeq, luego todos los DBIRTH; reinicia `dev_born` |
| SPB-10 | ✅ | `gw_node.c:300-335`; `gw_core/src/gw_poll.c:132-194` | Por nombre (por dispositivo) o por alias (rango del dispositivo). Rechaza desconocidos y de solo lectura con log, convierte escala y tipo, y relee antes del DDATA. Ver L2 (NaN en coil) |
| SPB-11 | ✅ | `gw_node.c:266-298` | `DO/n` por nombre o alias, con NDATA de confirmación aunque falle el expansor. `Reboot` publica NDEATH y reinicia |
| SPB-12 | ✅ | `gw_core/src/gw_rbe.c` | Deadband absoluta contra el **último valor reportado** (sin deriva). NaN y validez tratados |
| MB-1 | ✅ | `gw_rs485/gw_rs485.c:76-89`; `gw_config.c:320-322` | 1200–115200 baudios, N/E/O, 1 o 2 bits de stop, 8 bits de datos. Ver M2 sobre 1200 baudios |
| MB-2 | ✅ | `gw_rs485.c:91` | `UART_MODE_RS485_HALF_DUPLEX` con RTS = 21. Solo compilado (QEMU no lo modela) |
| MB-3 | ✅ | `gw_core/src/mb_rtu.c:51-62`; `gw_poll.c:10-22` | FC01–FC04 |
| MB-4 | ⚠️ | `gw_poll.c:146-173`; `gw_rs485.c:35` | FC05/FC06 para valores simples y **FC16 siempre para 32 bits**: correcto. Pero a 1200 baudios toda FC16 falla (M2) |
| MB-5 | ✅ | `gw_core/src/gw_conv.c:78-117` | Seis tipos, ABCD/CDAB. Mapeo a datatype de §6 (`:56-76`) |
| MB-7 | ✅ | `gw_conv.c:119-164` | Inversa `(v − offset)/scale`, redondeo, **rechazo por rango (sin saturación silenciosa)** y rechazo de NaN e infinitos. Funciona con escala negativa; `scale = 0` lo rechaza la configuración |
| MB-8 | ✅ | `gw_core/src/mb_master.c:18-34`, `:42-112` | t3,5 de 11 bits por carácter (1750 µs por encima de 19200), retardo entre tramas, timeout hasta el primer byte, reintentos. Las excepciones no se reintentan. CRC, eco y longitud validados. Basura o tramas parciales → `MB_ERR_FRAME` con drenaje |
| MB-9 | ⚠️ | `gw_poll.c:81-107`; `gw_core/src/gw_devstate.c` | `offline_after` ciclos seguidos → DDEATH; la primera respuesta → DBIRTH. **Pero un fallo parcial del ciclo deja valores viejos como válidos** (M1) |
| MB-10 | ✅ | `gw_poll.c:50-71`, `:125-128` | Periodo por dispositivo, sin ráfagas de recuperación |
| MB-16 | ✅ | `gw_bus/gw_bus.c:42-68` | Una tarea (`rs485_bus`) dueña de `mb_master_t`; las escrituras llegan por cola |
| IO-1 | ✅ | `gw_board/gw_io.c:159-200`; `gw_core/src/gw_debounce.c` | Muestreo a 1 kHz con `esp_timer` y debounce por integración configurable. Polaridad por Kconfig |
| IO-2 | ✅ | `gw_io.c:173-186`, `:206-222` | Driver TCA9554 propio: latch a 0 **antes** de pasar a salida, sin pulso de firmware, y relectura del registro de entrada. Ver L3 |
| IO-5 | ✅ | `gw_board/gw_status.c:38-58` | Cinco patrones documentados y priorizados. RMT por `led_strip`. Solo compilado |
| CFG-1 | ⚠️ | `gw_core/src/gw_config.c`; `gw_storage.c:17`, `:73-85`, `:104-116` | El validador es excelente: claves desconocidas, rutas JSON en los errores, todas las reglas de §6 y los límites 16/64/256. **Pero si el archivo existe y no se puede leer, o supera 32 KB, se usa la configuración embebida** (H1) |
| CFG-2 | ✅ | `gw_storage.c:101-118`; `main/CMakeLists.txt` | Embebida por `EMBED_TXTFILES` y sobrescribible por LittleFS (`/cfg/config.json`) |
| FW-4 | ✅ | `idf/CMakeLists.txt:3-20`; `gw_node.c:110` | `0.1.0+g<hash>[.dirty]`. Fuera de git queda `+unknown` |
| REL-1 | ⚠️ | `gw_bus.c:45-48`; `gw_edge.c:356-360`; `gw_rs485.c:68-72`; `sdkconfig.defaults` | TWDT de 15 s con pánico en `rs485_bus`, `sparkplug` e IDLE de los dos núcleos; el bus se alimenta en cada intento Modbus. **Pero el bloqueo acumulado de `sparkplug` en un rebirth no está acotado por debajo del TWDT** (M3). La tarea `esp_timer` (DI) no está vigilada |
| REL-2 | ✅ | Todo `gw_*` | Límites estáticos y sin `malloc` propio en encuesta ni publicación. cJSON y LittleFS asignan solo al arrancar. lwIP y esp-mqtt asignan internamente (el README lo declara como parcial) |
| REL-3 | ✅ | `esp_log` y `GW_LOG*` | Niveles consistentes; los errores de configuración llevan la ruta JSON |
| QA-1 | ✅ | `idf/test/host/` | 54 casos cubren todo lo que pide QA-1 |
| QA-2 | ✅ | — | 0 warnings y tamaño reportado; ambos reproducidos |

## 4. Hallazgos (por severidad)

| # | Sev. | Ubicación | Hallazgo y escenario de fallo concreto |
|---|---|---|---|
| H1 | **Alta** | `gw_storage/gw_storage.c:17` (`CFG_MAX 32 KB`), `:73-77`, `:82-85`, `:92-96`, `:104-116` | **Configuración válida ignorada y reemplazada por la de ejemplo.** `read_override()` devuelve NULL en cinco casos: archivo de más de 32 KB, error de lectura, fallo de `malloc`, LittleFS que no monta o archivo ausente. En **todos** ellos `gw_storage_load_config` usa el JSON embebido, que es el de §6: `Plant1`/`GW01`, broker 192.168.1.10, encuesta a la unidad 1. **Escenario:** 16 dispositivos × 16 tags con los campos del ejemplo de §6 da **39 085 B en JSON compacto** (75 KB con sangría), medido con un script. Es una configuración legal según los límites del MVP (256 tags). Se rechaza por tamaño con un solo `ESP_LOGE` y el gateway arranca con otra identidad. Si existe otro `GW01` en el broker, el client_id duplicado hace que el broker desconecte al otro, y los dos se expulsan en bucle. Viola CFG-1 («si es inválida no se arranca») y contradice el README §3.3. **Solución:** distinguir «ausente» (respaldo embebido) de «presente pero ilegible o demasiado grande» (error y nada arranca), y llevar `CFG_MAX` al peor caso del esquema (≈ 64–96 KB, leído en PSRAM) |
| M1 | **Media** | `gw_poll.c:86-91`, `:97-107` | **Valores congelados y válidos tras un fallo parcial del ciclo.** Si un bloque responde y uno posterior agota los reintentos, el `break` deja los tags restantes con el valor y el timestamp del último ciclo bueno y con `valid = true`, y `answered = true` mantiene el equipo en línea. RBE no publica nada. **Escenario** (reproducido con `probe_partial.c`): el esclavo responde FC01 y FC03 pero no FC04, por ejemplo por un bloque grande que excede su tiempo de respuesta o un mapa parcial que no contesta. `Voltage` queda en 230,5 «GOOD» indefinidamente mientras el valor real cambió. **Solución:** marcar `valid = false` en los miembros de los bloques no leídos, o contar el ciclo como fallido si falta algún bloque |
| M2 | **Media** | `gw_rs485/gw_rs485.c:35` | **A 1200 baudios toda escritura FC16 falla y se repite.** Con `tx_buffer_size = 0`, `uart_write_bytes` vuelve en cuanto la trama entra al FIFO. Después `uart_wait_tx_done(…, 100 ms)` debe cubrir la trama entera: se verificó en `esp_driver_uart/src/uart.c:1558-1598`. Una FC16 de 2 registros tiene 13 bytes: 108 ms en 8N1 y 119 ms con paridad o 2 bits de stop. Eso supera los 100 ms y devuelve `MB_ERR_FRAME` sin leer la respuesta. El esclavo **sí ejecuta** la escritura y se reintenta 2 veces más, así que la escritura se aplica 3 veces y el DCMD se reporta como fallido. Con paridad, la FC16 forzada de 1 registro (11 B, 101 ms) también falla. 1200 baudios está dentro de MB-1. **Solución:** timeout proporcional a `len × char_us` más un margen |
| M3 | **Media** | `gw_edge/gw_edge.c:26`, `:266-278`, `:319-335`; `gw_core/src/gw_node.c:143-155` | **El TWDT puede disparar durante un rebirth sobre un enlace lento.** `NET_TIMEOUT_MS = 5000` se justifica en un comentario como «acotado por debajo del watchdog», pero eso vale para **una** escritura de socket. En una sola vuelta de la tarea, `on_connected` hace 2 SUBSCRIBE más NBIRTH más hasta 16 DBIRTH (hasta unos 4,5 KB cada uno, fragmentados en el búfer de 2 KB). `reconcile` puede además emitir 16 DDATA, y nada alimenta el TWDT entre ellas. Con `TCP_SND_BUF` de 5760 B, un enlace lento pero vivo (WAN o celular con retransmisiones) puede tardar 1–4 s por escritura sin llegar a abortar, y superar los 15 s → **pánico y reinicio** (`CONFIG_ESP_TASK_WDT_PANIC=y`). El siguiente arranque repite el mismo rebirth, lo que da un posible bucle de reinicios con bdSeq nuevo cada vez. **Solución:** `esp_task_wdt_reset()` por cada publish (en `op_publish`), o un presupuesto de tiempo por vuelta |
| L1 | Baja | `gw_edge.c:284-298` | **Carrera en `reconcile`.** Lee `s_mqtt_connected` (`:285`) **antes** de `s_conn_gen` (`:292`). La tarea esp-mqtt no tiene afinidad (`CONFIG_MQTT_TASK_CORE_SELECTION_ENABLED` no está activo) y puede correr en el núcleo 1. Si `MQTT_EVENT_CONNECTED` llega entre las dos lecturas, la generación se consume con `connected = false` y `on_connected()` no se llama nunca. La sesión MQTT queda sana pero **sin NBIRTH ni suscripciones, sin reintento y sin salida**, porque los NCMD se ignoran con `!online`. La ventana es de nanosegundos, pero no hay recuperación. **Solución:** leer primero la generación y luego el flag |
| L2 | Baja | `gw_poll.c:147`; `gw_core/src/spb.c:223-225` | Un DCMD Float **NaN** sobre un tag coil se escribe como **ON**, porque `NaN != 0.0` es verdadero. Para registros sí se rechaza (`gw_conv.c:121`) |
| L3 | Baja | `gw_io.c:175-186`, `:208-210` | Si el TCA9554 no responde en el arranque (por ejemplo, un glitch de I²C), las DO quedan deshabilitadas hasta el próximo reinicio. No hay reintento ni reintegración posterior |
| L4 | Baja | `gw_storage.c:53-55`; sdkconfig: `# CONFIG_UART_ISR_IN_IRAM is not set` | En modo RS485 el driver libera RTS en la ISR de `TX_DONE`. El `nvs_commit` de cada intento MQTT deshabilita la caché, y una ISR que no está en IRAM se difiere. Si coincide con el final de una petición Modbus, el DE queda activo mientras el esclavo responde → se pierde la respuesta y se reintenta. Se recupera solo, pero es evitable con `CONFIG_UART_ISR_IN_IRAM=y` |
| L5 | Baja | `proto/sparkplug_b.options` (`name`/`string_value max_size:64`); `spb.c:128-134` | En un NCMD o DCMD con un nombre o un string de más de 63 caracteres, nanopb aborta el decode **a mitad del mensaje**. Las métricas anteriores ya se aplicaron (por ejemplo, DO) y las siguientes se pierden: aplicación parcial de un comando |
| L6 | Baja | `gw_edge.c:268-269` | No se comprueba el resultado de `esp_mqtt_client_subscribe`. Si falla (outbox lleno o error de escritura que no aborta), el nodo publica NBIRTH pero no recibe NCMD ni DCMD (ni Rebirth) hasta la próxima reconexión |
| L7 | Baja | `gw_edge.c:234-245`, `:311-314` | Si `esp_mqtt_client_init` falla (solo por falta de memoria), `start_client` se reintenta cada vuelta (≤ 1 s) y cada intento consume un bdSeq y escribe NVS: desgaste de flash sin backoff |
| L8 | Baja | `gw_node.c:262-263` | Cosmético: el log de descarte imprime `alias 0` cuando el comando no traía alias (visto en el E2E: `metric 'Voltage' (alias 0)`) |
| L9 | Baja (doc.) | `idf/README.md` §1.2 y §2 | Dice que lwIP y esp_eth están en el núcleo 0, pero `CONFIG_LWIP_TCPIP_TASK_AFFINITY_NO_AFFINITY=y` y esp-mqtt tampoco tiene afinidad. La lista de pines de *strapping* omite **GPIO3** (JTAG; es el RX del CAN en la placa, que el MVP no usa) |

Se verificaron y **no** son defectos:

- `mb_response_length` puede pedir hasta 260 B, pero `mb_master.c:62` corta en `need > sizeof(rx)`.
- `enqueue_cmd` acota el topic y el payload (`gw_edge.c:135-140`).
- Los paths de error de la configuración usan `snprintf` y `strncat` acotados.
- El backoff no desborda (`uint64` con tope).
- Las conversiones float → int verifican el rango antes de castear.
- La escala negativa funciona.
- `esp_mqtt_client_reconnect` solo actúa en `WAIT_RECONNECT`, y el código reprograma si falla.
- `esp_mqtt_abort_connection` se dispara desde la tarea `sparkplug` en un fallo de escritura y no produce deadlock: el handler solo hace `xQueueSend(…, 0)`.
- **El outbox lleno** afecta solo a QoS > 0 (NDEATH y SUBSCRIBE). QoS 0 va directo al socket. El límite de 8 KB evita crecer sin límite.
- **Broker caído o half-open:** el keepalive de esp-mqtt aborta, el backoff es correcto y no hay tormenta de reconexión.

## 5. Aprovechamiento del hardware

| Aspecto | Valoración |
|---|---|
| **UART en modo RS485 por hardware** | ✅ `UART_MODE_RS485_HALF_DUPLEX` con RTS = GPIO21 (`gw_rs485.c:91`), la vía preferida por MB-2. Es «semi-hardware»: el DE lo suelta la ISR de `TX_DONE`, y esa ISR no está en IRAM (L4). `uart_set_rx_timeout(3)` es adecuado. Sin verificar en placa (QEMU no lo modela) |
| **W5500** | ✅ Por **interrupción** (INT = 12, `poll_period_ms = 0` del `ETH_W5500_DEFAULT_CONFIG`), RST = 39, SPI2 a 20 MHz con DMA, MAC del eFuse (el W5500 no trae MAC). Bien resuelto |
| **PSRAM octal** | ⚠️ Habilitada (`SPIRAM_MODE_OCT`, 80 MHz, `IGNORE_NOTFOUND`, `USE_MALLOC`), pero **nada se coloca en ella deliberadamente**. Los 80 KB de `.bss` (configuración de 26 KB, `gw_node_t` de 27 KB) están en DIRAM interna. Para el MVP sobra RAM (44 %), pero es la palanca obvia para TLS y SPB-14 (`EXT_RAM_BSS_ATTR` para `gw_config_t`) y para subir `CFG_MAX` (H1) |
| **Afinidad y prioridades** | ✅ `rs485_bus` en el núcleo 1, prioridad 10 (lejos de la red); `sparkplug` en el núcleo 0, prioridad 8; LED con prioridad 2. Pilas estáticas (`xTaskCreateStaticPinnedToCore`). lwIP y esp-mqtt sin afinidad (L9) |
| **WS2812** | ✅ `espressif/led_strip` 3.1.1 sobre RMT a 10 MHz, orden RGB, brillo limitado. Refresca cada 125 ms aunque no cambie, lo que es inocuo |
| **TCA9554 y pulsos de las DO** | ✅ Driver mínimo propio, justificado: el componente oficial pone el latch en 0xFF. Escribe primero el latch (OUTPUT = 0) y luego CONFIG = 0 (salida), lo correcto para que el **firmware** no genere pulsos en arranque en frío ni en caliente. La relectura sale del registro INPUT. Límite físico no tratado: entre el encendido y `app_main` los pines son entradas con el pull-up interno débil del TCA9554 (100 kΩ según la hoja de datos). Previsiblemente no basta para excitar el opto, pero solo se confirma en placa. Sin reintento si falta el expansor (L3) |
| **DI** | ✅/⚠️ Muestreo a 1 kHz en `esp_timer` (tipo tarea), que cumple IO-1 («interrupción o muestreo ≥ 1 kHz»). No usa interrupciones de GPIO. Hace 8 `gpio_get_level` por ms en lugar de una sola lectura de `GPIO_IN_REG` (o *dedicated GPIO*). Debounce por integración correcto; pull-up interno coherente con la polaridad activa en bajo |
| **Pines de *strapping*** | ✅ El MVP no toca GPIO0, 3, 45 ni 46. Tampoco choca con la PSRAM octal (GPIO33–37) ni con el USB (19/20). La documentación omite GPIO3 (L9) |
| **Hechos de la placa** | Razonablemente fundados: demo oficial de Waveshare más ESPHome de la placa hermana, con nivel de confianza declarado. TCA9554 en 0x20 es la dirección por defecto con A0–A2 a GND, y aparece en dos fuentes. La polaridad de las DI (pull-up y opto que tira a GND) es plausible y está **parametrizada por Kconfig** (`GW_DI_ACTIVE_LOW`), lo que mitiga el riesgo. El orden RGB del LED tiene confianza media. Este revisor no volvió a descargar el demo; los hechos siguen sin verificar en hardware |

## 6. Calidad de código e idiomaticidad ESP-IDF

**Estructura.** Hay 9 componentes con responsabilidades nítidas:

- `gw_core` es puro, con un `port/` de cuatro funciones (lock, tiempo, log) y dos implementaciones, ESP y POSIX;
- `gw_rs485`, `gw_bus`, `gw_edge`, `gw_net`, `gw_board` y `gw_storage` son el pegamento con IDF;
- `main` solo cablea.

Los `REQUIRES` y `PRIV_REQUIRES` son mínimos y correctos. `gw_core/CMakeLists.txt` compila como componente IDF o como biblioteca de host, una técnica limpia.

**Kconfig y sdkconfig.**

- Cada componente tiene su menú `Gateway: …` con rangos.
- `sdkconfig.defaults` está comentado por requisito (REL-1, TIME-1, MQTT-7).
- `sdkconfig.qemu` es un overlay pequeño y documentado.
- `sdkconfig` está en `.gitignore`.

**esp_event.** Se usa donde corresponde: `ETH_EVENT`, `IP_EVENT` y el handler de esp-mqtt. La coordinación interna va por flags atómicos y colas FreeRTOS estáticas, en lugar de un bucle de eventos propio. Es simple, sin asignaciones, y está bien razonado (perder un *wake* no pierde estado).

**Errores.** `ESP_RETURN_ON_ERROR` y `ESP_ERROR_CHECK` en la inicialización, códigos propios en `gw_core` y degradación explícita: sin NVS se usa RAM, sin expansor no hay DO. Las fallas de arranque abortan (`ESP_ERROR_CHECK` en `app_main`), lo que es aceptable con el TWDT y el reinicio.

**Logs.** Tags por módulo y niveles coherentes. Los errores de configuración llevan la ruta JSON y se repiten cada 10 s.

**Legibilidad.** Muy buena:

- archivos cortos (el más largo es `gw_edge.c`, con 427 líneas);
- nombres expresivos y comentarios que explican el *porqué*;
- ninguna función larga difícil de seguir;
- invariantes documentados en los headers (quién llama a qué, desde qué tarea).

**Código muerto.** No se encontró:

- `primary_host_id` y `units` se validan pero no se usan, y está documentado;
- `SPB_DT_DATETIME` y `TEXT` en el codec están de más, pero son inocuos.

**Detalles menores.**

- `gw_config_tag_device` devuelve 0 ante un índice fuera de rango, lo que enmascara errores.
- El log de alias es engañoso (L8).
- Queda un `tools/__pycache__/*.pyc` suelto en el árbol de trabajo, sin versionar.

**Tamaño (sin blancos ni comentarios, contado con un script).**

| Categoría | Líneas brutas | Líneas efectivas |
|---|---|---|
| App — `gw_core` (lógica pura + ports) | 3 041 | 2 531 |
| App — componentes IDF + `main` | 1 582 | 1 308 |
| **App total** | **4 623** | **3 839** |
| Tests de host (`test/host`) | 2 016 | 1 771 |
| Herramientas (E2E, decoder, gen) | ≈ 665 | — |
| Vendorizado (nanopb 0.4.9.1 + `.pb.c/.h` generados) | 4 709 | 3 192 |
| Build (CMake, Kconfig, yml, sdkconfig*) | 441 | — |

Es un tamaño contenido para lo que cubre: maestro Modbus propio de unas 400 líneas en lugar de esp-modbus, con la decisión justificada en la viabilidad.

**Higiene de dependencias.**

- Todas con `==` en `idf_component.yml`, y `dependencies.lock` con versiones y hashes: mqtt 1.1.0, w5500 2.0.1, cjson 1.7.19~2, led_strip 3.1.1, littlefs 1.23.1, `idf >= 6.1, < 6.2`.
- nanopb está vendorizado tal cual, con licencia y `gen_proto.sh`.
- El proto es idéntico al de Tahu.
- Los tests de host fijan Unity y cJSON por *tag* de git, no por hash de commit (mejorable).
- pymodbus está fijado.

**Testabilidad.**

- La separación permite probar en el host el motor Sparkplug completo, el poller contra un esclavo simulado (con inyección de CRC, unidad errónea, excepción y fuera de línea) y el parser de configuración.
- Lo que no tiene test unitario es la gestión de conexión de `gw_edge`, justo donde están L1, L6 y M3. Solo la cubre el E2E.
- El E2E en QEMU con el firmware real es un plus notable para QA-3, fuera del MVP.

## 7. Scorecard

| Eje | Nota (1–10) | Justificación |
|---|---|---|
| Completitud MVP | **9** | Están todos los IDs del MVP. Tres ⚠️ por defectos concretos (CFG-1, MB-9, MB-4) y REL-1 con un hueco; ningún ❌ |
| Corrección Sparkplug | **9** | bdSeq, will, NBIRTH, seq, alias, rebirth, orden DBIRTH/DDEATH y DCMD correctos y verificados en el E2E contra el proto oficial. Solo la carrera L1 y la aplicación parcial L5 |
| Corrección Modbus | **7** | Framing, CRC, t3,5, excepciones, inversa de escala y elección de FC impecables. Los valores congelados del fallo parcial (M1) y FC16 a 1200 baudios (M2) son errores de campo reales |
| Robustez / potencial de bugs | **7** | Memoria estática, colas acotadas y backoff correcto. Pero la configuración se sustituye en silencio (H1), el bloqueo acumulado frente al TWDT (M3) y la carrera sin recuperación (L1) |
| Aprovechamiento del hardware | **7** | RS485 de la UART, W5500 por INT, RMT y TCA9554 sin pulso. PSRAM sin uso deliberado, ISR de la UART fuera de IRAM, DI por muestreo y nada probado en placa |
| Idiomaticidad del stack | **8** | Componentes, Kconfig, versiones fijas con lock, esp_netif, esp_event, primitivas FreeRTOS estáticas y `ESP_RETURN_ON_ERROR`. El uso de esp-mqtt (reconexión manual) está bien estudiado |
| Legibilidad / elegancia | **9** | Módulos pequeños, invariantes documentados y un diseño de concurrencia simple y explicado. Poco que objetar |
| Testabilidad / evidencia | **8** | 54 tests de host, interoperabilidad con el proto oficial y E2E QEMU 22/22, todo reproducido. Sin tests de `gw_edge` ni de fallos parciales, y sin hardware |
