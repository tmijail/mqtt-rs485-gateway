# Revisión independiente — implementación Zephyr del gateway RS485 → Sparkplug B

Revisor: revisión independiente y escéptica (2026-11-03). Alcance: `zephyr/`, `docs/feasibility-zephyr.md` y `docs/SPEC.md`.
No se leyó `idf/` ni `docs/feasibility-idf.md`. No se modificó código: la única escritura es este archivo.
Los artefactos de la reproducción están en el scratchpad de la sesión (`.../scratchpad/zrev/`).

## 1. Resumen

La implementación es **sólida y honesta**. Las afirmaciones del `README.md` que se pudieron comprobar se
reprodujeron exactamente:

- build para la placa sin warnings de compilador y con el mismo uso de flash y RAM;
- 32/32 tests unitarios;
- 30/30 comprobaciones de punta a punta.

La semántica Sparkplug B es correcta en todos los puntos auditados:

- bdSeq persistido, incrementado por intento, y el mismo valor en el LWT y en el NBIRTH;
- seq de 0 a 255 que vuelve a 0, y NDEATH sin seq;
- QoS y retain correctos;
- aliases fijos;
- DBIRTH siempre después del NBIRTH;
- rebirth con seq=0 y el mismo bdSeq;
- DCMD por alias o por nombre, con descarte de métricas de solo lectura o inexistentes.

El código es idiomático de Zephyr: placa HWMv2 propia, `zephyr,user` para la E/S, Kconfig, settings+ZMS,
`conn_mgr`, `task_wdt` con respaldo de hardware y `eventfd`+`poll`. La lógica pura está bien separada del
HAL.

Los problemas reales están en la **robustez de la sesión MQTT**, en un **agujero del validador de
configuración** y en la **política del maestro Modbus con equipos caídos**:

1. **No se detecta la falta de PINGRESP** (`unacked_ping` no se consulta), y la pérdida de red L4 no
   aborta la sesión. Lo verifiqué: con el broker congelado durante 60 s (6× keepalive), el gateway seguía
   en `mqtt: connected`.
2. **Una clave `"tags"` duplicada se acepta**: deja rangos de tags solapados entre dispositivos, aliases
   repetidos en dos DBIRTH y un tag "fantasma" a ceros. Lo verifiqué con un arnés en el host.
3. **Los dispositivos fuera de línea consumen unos 0,65 s de bus por ciclo** con los valores por defecto,
   lo que degrada la encuesta de los demás.

Además, nada se ejecutó en hardware real. La evidencia del W5500, el DE, el TCA9554 y el LED es solo de
compilación, y el README lo dice sin disimulo.

## 2. Reproducción

Workspace `/home/user/sdk/zephyr/ws` (Zephyr 4.5.0, `41d10b597`), SDK 1.0.1 y `-j2`. Usé directorios de
build propios y, para la e2e, puertos y ptys propios. Otro revisor compilaba ESP-IDF y QEMU en paralelo.

| Paso | Comando (resumido) | Resultado |
|---|---|---|
| Firmware ESP32-S3 | `west build -p -b esp32s3_poe_eth_8di_8do/esp32s3/procpu zephyr -d <scratch>/build-board -o=-j2` | **OK (RC=0)**. **0 líneas `warning:` del compilador.** Hay 1 `CMake Warning` del árbol de Zephyr (`No SOURCES given to Zephyr library: drivers__net`), inocuo y ajeno al código propio |
| Tamaño | salida del enlazador | FLASH 422 308 B (2,52 % de 16 MB) · DRAM `dram0_0_seg` 217 480 / 399 108 B (**54,5 %**) · IRAM 66 568 / 415 492 B (16,0 %). Coincide byte a byte con el README |
| Tests unitarios (QA-1) | `west twister -T zephyr/tests/unit -p native_sim -j 2 -O <scratch>/twister-out` | **32/32 PASS**, 1/1 configuración, "with no warnings" |
| Build native_sim | `west build -b native_sim ... -DGW_CONFIG_FILE=<copia de e2e_config.json con puerto 18931>` | OK, 0 warnings |
| E2E (QA-3) | Copia de `run_e2e.py` en el scratchpad con broker en `:18931`, ptys `/tmp/zrev-gw-*` y `--rs485_port`; la lógica no se tocó | **30/30 PASS**. DDEATH en 3,25 s (cota 3,8 s), DBIRTH al volver en 0,62 s, DI→NDATA en 25 a 30 ms, NCMD DO/3 en 49 ms, reconexión tras reiniciar el broker en 2,6 s (bdSeq 0 → 3), NDEATH con el bdSeq del NBIRTH, bdSeq 3 → 4 tras reiniciar, 33 mensajes decodificados con el proto oficial y 0 errores. `FW-4 = 0.1.0+8134a54a4c36` |
| Experimento propio: broker colgado | mosquitto en `:18931` + `zephyr.exe`; tras el NBIRTH, `kill -STOP` al broker durante 60 s (keepalive 30 s) | **El gateway siguió en `mqtt: connected` los 60 s.** Solo reconectó (`session error (-104)`) cuando el broker reanudó y cerró él mismo la conexión. Confirma el hallazgo M1 |
| Experimento propio: `"tags"` duplicado | `config.c` + `json_reader.c` compilados con gcc en el host, con un JSON con dos claves `"tags"` en un dispositivo | `ret=0`. Dispositivo A: `first=2, count=3`, que abarca el tag `b1` del dispositivo B (alias 1003 duplicado) y un tag índice 4 con `name=''` y `scale=0`. Confirma el hallazgo M2 |

## 3. Auditoría de los IDs del MVP

Evidencia: **U** = test unitario reproducido, **E** = e2e reproducida, **B** = solo compila (no se ejecutó en la placa), **R** = lectura de código.

| ID | Nota | Evidencia | Referencia y comentario |
|---|---|---|---|
| NET-1 | ⚠️ | B, R | DTS `procpu.dts:138-144`: `wiznet,w5500` con `int-gpios` GPIO12 y `reset-gpios` GPIO39. El driver usa la INT (`eth_wiznet.c:478`) y monitoriza el enlace. Pero `netmgr.c:59-63` solo marca el estado al perder L4: la sesión MQTT viva no se aborta (M1). Sin prueba en hardware |
| NET-2 | ✅ | B, R | `netmgr.c:115-151`: DHCP (`net_dhcpv4_start`) o estática con máscara, gateway y DNS |
| NET-3 | ✅ | E (IP numérica), R | `mqtt_link.c:190-206` (`zsock_getaddrinfo`), DNS de DHCP (`CONFIG_NET_DHCPV4_OPTION_DNS_ADDRESS=y`) o manual (`netmgr.c:140-149`). Una IP estática sin `dns` se acepta (L7) |
| TIME-1 | ✅ | E | `gw_time.c:38-62` (`sntp_simple` + `sys_clock_settime`), resincronización periódica en `netmgr.c:172-180`, espera de 5 s antes del primer NBIRTH (`mqtt_link.c:417-420`). Timestamps en ms UTC |
| MQTT-1 | ✅ | E (sin credenciales), R | `mqtt_link.c:208-233`: host, puerto, client_id (por defecto `edge_node_id`), usuario, contraseña y keepalive. 3.1.1 y `clean_session=1` |
| MQTT-2 | ✅ | U, E | `backoff.h` (de 1 a 60 s, *equal jitter*), `mqtt_link.c:390-444`. Ver L1 (tormenta si el broker acepta y corta) |
| MQTT-3 | ✅ | E | `mqtt_link.c:235-246`: will = NDEATH, QoS 1, `will_retain=0` |
| MQTT-7 | ✅ | R | Estado coalescido (`model.c`) en lugar de cola de publicación. Solo hay una `k_msgq` acotada para DCMD (`mb_poller.c:37`). El payload se envía sin copiar a `tx_buf` |
| SPB-1 | ✅ | U, E | `sparkplug.c` + `proto/` (Tahu) con nanopb. 33/33 mensajes decodificados con el proto oficial |
| SPB-2 | ✅ | E | LWT (`mqtt_link.c:241-246`); NDEATH explícito antes del reinicio (`mqtt_link.c:339-351`); `spb_node.c:461-472` (un solo `bdSeq` UInt64) |
| SPB-3 | ✅ | E | `bdseq.c:50-61`: se persiste *antes* de conectar, en cada intento (`mqtt_link.c:430-434`). El mismo valor va al LWT y a `spb_node_session_start`. Comprobado tras reiniciar (3 → 4) |
| SPB-4 | ✅ | U, E | `spb_node.c:282-308`: seq=0, timestamp, 21 métricas con nombre, alias, datatype y valor. QoS 0 y `retain_flag=0` (`mqtt_link.c:110`) |
| SPB-5 | ✅ | U, E | `spb_node.c:143-146` (`uint8_t` que vuelve a 0), `n.seq = 0` en el NBIRTH (`:297`), NDEATH con `has_seq=false` (`:465`). La e2e vio 17 mensajes sin huecos |
| SPB-6 | ✅ | U, E | `spb_node.c:349-390, 427-447`: reconciliación por generación. El DBIRTH solo sale después del NBIRTH (`:501-522`), con todas las métricas y su valor actual; los tags sin leer van con `is_null` |
| SPB-7 | ✅ | U, E | `spb_node.c:211-221, 392-424`: solo alias, sin nombre ni datatype, con timestamp de adquisición y un mensaje por dispositivo y ciclo |
| SPB-8 | ✅ (*) | U, E | `spb_node.h:24-31`: los aliases se derivan del índice global, así que son únicos *si la configuración es coherente*. (*) Con `"tags"` duplicado deja de serlo (M2) |
| SPB-9 | ✅ | U, E | `spb_node.c:501-507, 572-576`: NBIRTH con seq=0 y el mismo bdSeq, y después todos los DBIRTH |
| SPB-10 | ✅ | U, E | `spb_node.c:593-644`: alias o nombre, rechaza las métricas desconocidas y las de solo lectura (con log), conversión inversa y relectura de confirmación (`mb_poller.c:133-168`). Si la escritura falla o el equipo está fuera de línea, no hay respuesta al host (solo log) |
| SPB-11 | ✅ | U, E | `spb_node.c:577-587`; `local_io.c:89-120` (la confirmación es el ACK I²C); reinicio con NDEATH ordenado |
| SPB-12 | ✅ | U, E | `model.c:64-73` (banda absoluta estricta, NaN tratado), `model.c:75-85` (se mide la deriva desde el último valor reportado) |
| MB-1 | ✅ | E (9600 8N1), R | `mb_poller.c:300-341`, `CONFIG_MODBUS_NONCOMPLIANT_SERIAL_MODE` para respetar los bits de stop, `UART_USE_RUNTIME_CONFIGURE=y` (necesario: sin él `modbus_serial_init` ignora baud y paridad) |
| MB-2 | ✅ (alternativa) | B, R | `de-gpios` GPIO21 (`procpu.dts:116-127`). Está bien justificado por qué no se usa el modo HW: el binding usa DTR (`espressif,esp32-uart.yaml:27-33`) y `configure_uart` fuerza `FLOW_CTRL_NONE`. Es la alternativa que el spec permite |
| MB-3 | ✅ | E | `mb_poller.c:68-97`: FC01, 02, 03 y 04 |
| MB-4 | ⚠️ | E | FC05, FC06 y FC16, con FC16 siempre para 32 bits (`mb_poller.c:77-92`). **FC15 no se emite nunca** (reconocido en el README) |
| MB-5 | ✅ | U, E | `convert.c`: los 6 tipos, ABCD y CDAB |
| MB-7 | ✅ | U, E | `convert.c`: inversa `(v-offset)/scale` con `round()` y rechazo (no saturación) fuera de rango con `-ERANGE`. Funciona con scale negativa; `scale=0` se rechaza en la configuración |
| MB-8 | ✅ | E, R | Timeout = `rx_timeout` del subsistema (incluye el tiempo de TX de la petición), reintentos (`mb_poller.c:102-131`), retardo entre transacciones (`:58-63`). t3,5 en RX: temporizador RTU del subsistema (ver L8) |
| MB-9 | ✅ | E | `mb_poller.c:198-243`. Una excepción cuenta como respuesta. Al volver, se lee el dispositivo entero antes del DBIRTH. Ver L4 y M3 |
| MB-10 | ✅ | E | `next_due` por dispositivo; si hay retraso, se reinicia el periodo en lugar de encadenar ráfagas (`mb_poller.c:282-286`). Degradación con equipos caídos: M3 |
| MB-16 | ✅ | R, E | Un solo hilo `mb_poller` hace todas las transacciones; las escrituras le llegan por `k_msgq` |
| IO-1 | ✅ | U, E | Muestreo a 1 kHz con `k_timer` (`local_io.c:57-87`) y debounce por contador (`debounce.c`). La DI aparece en NDATA a los 25-30 ms |
| IO-2 | ✅ | E (gpio-emul), B | `local_io.c:89-142`; DTS `ti,tca9554` en 0x20, activo bajo |
| IO-5 | ✅ | U, B | `status_led.c:32-47`: prioridades y colores documentados; WS2812 por RMT |
| CFG-1 | ⚠️ | U, E | Validador estricto con rutas JSON, línea y columna (`config.c`); sin configuración válida no hay encuesta ni MQTT (`main.c:76-80`). **Las claves duplicadas no se detectan** (M2) |
| CFG-2 | ✅ | E (README), R | `app_config.c`: el archivo en `/lfs/config.json` tiene prioridad sobre la configuración embebida (`GW_CONFIG_FILE`) |
| FW-4 | ✅ | E | `VERSION` + `APP_BUILD_VERSION` → `0.1.0+<hash>` |
| REL-1 | ✅ | B, R | `task_wdt` en los 4 hilos y `wdt0` como respaldo: `TASK_WDT_HW_FALLBACK=y`, ventana HW de 120 ms alimentada por el temporizador de `task_wdt`. No cubre la workqueue del sistema, que procesa las tramas Modbus (L3) |
| REL-2 | ✅ | R | Todo estático y acotado por Kconfig. La única alocación está en `getaddrinfo` (al conectar), no en la encuesta ni en la publicación |
| REL-3 | ✅ | E | Logging de Zephyr por módulo, nivel `CONFIG_GW_LOG_LEVEL` |
| QA-1 | ⚠️ | U | Hay tests de Sparkplug, conversión, RBE y configuración. **No hay tests de CRC ni de tramas Modbus**: se delegan al subsistema y solo se demuestran en la e2e. `mb_poller` (reintentos, fuera de línea, relectura) no tiene test unitario |
| QA-2 | ✅ | reproducido | 0 warnings de compilador; tamaños reportados |

## 4. Hallazgos (ordenados por severidad)

No encontré ningún hallazgo **Alto**: ningún fallo que, en condiciones normales, corrompa datos o deje el
equipo sin recuperación.

| # | Sev. | Hallazgo | Escenario concreto de falla | Referencia |
|---|---|---|---|---|
| M1 | **Media** | **Sesión MQTT medio abierta sin detectar.** No se consulta `client.unacked_ping`: la biblioteca de Zephyr lo cuenta pero no actúa (`subsys/net/lib/mqtt/mqtt.c:626-629`, `mqtt_live` en `:705-730`). La pérdida de L4 tampoco aborta la sesión | El broker se cuelga, o hay un firewall o NAT que descarta el estado: el gateway sigue "conectado" y publica DATA QoS 0 al vacío. **Verificado:** 60 s con el broker congelado sin reconectar. Si el par muere sin RST, solo se detecta por el RTO de TCP (`NET_TCP_RETRY_COUNT=9`, RTO inicial de 200 ms: unos 3 min), frente a los 1,5×keepalive esperables. Mientras tanto, el host ya recibió NDEATH | `mqtt_link.c:364-386`, `netmgr.c:59-63` |
| M2 | **Media** | **Las claves JSON duplicadas se aceptan.** Un segundo `"tags"` en un dispositivo reinicia `first_tag` sin reiniciar `tag_count` | Un `"tags"` repetido por un copiar y pegar: el dispositivo A pasa a abarcar tags del B, el alias 1003 aparece en dos DBIRTH (rompe SPB-8), A encuesta la dirección de `b1` en su propio `unit_id` y acepta DCMD sobre ella (es escribible), y se añade un tag a ceros (`name=""`, `scale=0`, coil 0). **Verificado con un arnés en el host.** El comentario de `json_reader.c:2-3` dice que el llamador controla los duplicados, y no lo hace | `config.c:545`, `config.c:595-597`, `json_reader.c:2-3` |
| M3 | **Media** | **Los dispositivos fuera de línea saturan el bus.** Cada ciclo de un equipo caído cuesta (retries+1)×(timeout+trama+delay), unos 3×215 ms ≈ 0,65 s con los valores por defecto, y se sondea a su `poll_ms` completo, sin espaciar | Con 16 equipos a `poll_ms=1000` y 2 apagados, el bus queda ocupado más de 1,3 s por segundo. La lógica de retraso (`:282-286`) estira el periodo de todos los equipos en línea (MB-10) y la latencia de DCMD crece. En planta, tener equipos apagados es lo habitual | `mb_poller.c:102-131`, `198-243` |
| L1 | Baja | **Tormenta de reconexión.** `attempt` vuelve a 0 con el CONNACK, no después de una sesión estable | Un broker que acepta y corta (client_id duplicado con otro equipo, ACL): un bucle de unos 1 s, cada vuelta con NBIRTH y todos los DBIRTH, y **una escritura en flash del bdSeq por intento** | `mqtt_link.c:433`, `bdseq.c:50-61` |
| L2 | Baja | **Watchdog del hilo MQTT.** Se alimenta una vez por vuelta de servicio, y una vuelta puede publicar NBIRTH y 16 DBIRTH con `SO_SNDTIMEO` de 10 s por envío | Con un broker o una red muy lentos que avanzan a cuentagotas, la suma supera los 30 s y `task_wdt` reinicia el equipo. El mecanismo está verificado; el escenario es improbable | `mqtt_link.c:43`, `:315`, `:367-368` |
| L3 | Baja | **La workqueue del sistema queda fuera del watchdog.** Ahí corre `modbus_rx_handler` (`modbus_core.c:32-40`, sin `CONFIG_MODBUS_WORKQUEUE`) | Si la workqueue se bloquea, todos los equipos pasan a fuera de línea (DDEATH en cascada) y el equipo no se reinicia, porque `mb_poller` sigue alimentando su canal | `prj.conf`, `main.c:43-52` |
| L4 | Baja | **Un tag mudo deja fuera de línea el dispositivo entero.** Un tag que nunca responde, ni siquiera con excepción, hace fallar todos los ciclos | Un esclavo no conforme con un registro "agujero": el dispositivo nunca llega a estar en línea. Es una decisión documentada en el README §6.5 | `mb_poller.c:218` |
| L5 | Baja | **Pulsos de DI perdidos.** Se publica el estado, no los eventos | La DI cambia y vuelve antes de que `mqtt_link` atienda, por ejemplo mientras está bloqueado enviando DBIRTH: no sale ningún NDATA. IO-4 (contadores) no es MVP | `spb_node.c:317-332` |
| L6 | Baja | **RAM reservada para siempre.** 32 KB de `file_buf`, que solo se usan en el arranque, más los 4 KB de la pila de `main`, que retorna | DRAM al 54,5 %, con PSRAM de 8 MB sin habilitar: es margen que faltará para TLS (MQTT-4) | `app_config.c:20`, `prj.conf:5` |
| L7 | Baja | **IP estática sin `dns` aceptada.** Con `dhcp:false` no se exige `dns` | Con un `mqtt.host` por nombre, el broker no resuelve nunca y el único síntoma es el log | `config.c:291`, `netmgr.c:140` |
| L8 | Baja | **Afirmación inexacta sobre t3,5.** El estudio dice que el subsistema usa un t3,5 "fijo por encima de 38400 baud, como pide la norma". La norma fija 1,75 ms por encima de 19200 baud; Zephyr calcula 11·3,5e6/baud hasta 38400 y por encima usa ≈1,0 ms | Por encima de 38400 baud, un esclavo con huecos entre caracteres de entre 1,0 y 1,75 ms (ya fuera de t1,5) vería sus tramas partidas. Es un riesgo menor y documental | `subsys/modbus/modbus_serial.c:674-679`, `feasibility-zephyr.md` (MB-8) |
| L9 | Baja | **Pilas sin medir ni proteger.** Pilas dimensionadas a mano (1,5, 3, 4 y 2,5 KB) sin `THREAD_ANALYZER`, `STACK_SENTINEL` ni `HW_STACK_PROTECTION` | El decode nanopb y los `snprintf` en el hilo MQTT no tienen el margen medido. Un desbordamiento sería silencioso | `local_io.c:37`, `mb_poller.c:38`, `mqtt_link.c:47`, `netmgr.c:47` |
| L10 | Baja | **Log inundado por timeouts.** El subsistema registra un `WRN` por cada timeout. Está reconocido | Con equipos apagados, el log diferido (4 KB) descarta mensajes útiles | `modbus_core.c:172` |

Revisé estos puntos y **no** encontré problema:

- carreras: el modelo va con mutex y los bits sucios son atómicos; `spb_node` corre en un solo hilo;
- desbordamientos en el codec: nanopb con tamaños fijos (nombre y string de 64), `copy_str` comprueba la
  longitud, y el payload de DCMD está acotado a 1 KB y se descarta con `discard_payload`;
- parseo de topics: `spb_topic_parse_cmd` comprueba las longitudes;
- conversiones float→int: hay rango con `-ERANGE` y `round()`;
- tramas basura o parciales: el subsistema comprueba el CRC y la longitud, y el cliente comprueba el
  `unit_id`, el FC y el número de bytes, todo con `-EIO`/`-EINVAL` y reintento;
- excepciones: se devuelven como código positivo, no se reintentan y cuentan como respuesta;
- glitch de las DO al arrancar: `gpio_tca9554.c:83-92` escribe el registro de salida *antes* de cambiar
  la dirección, y el valor de reset es 0xFF, que con lógica activa en bajo deja las salidas apagadas;
- reconfiguración del subsistema Modbus en caliente: no existe (`modbus_init_client` se llama una sola vez).

## 5. Uso del hardware

| Aspecto | Valoración |
|---|---|
| Definición de placa | **Buena.** HWMv2 propia (`boards/waveshare/...`), con el mapa de pines idéntico a §2.1: W5500 13/14/15/16, INT 12, RST 39 = `gpio1 7`; RS485 17/18/21; I²C 42/41; RTC INT 40 = `gpio1 8`; LED 38; buzzer 46; SD 48/47/45; CAN 2/3. Particiones de 16 MB pensadas para OTA A/B, LittleFS y settings. Lo que no usa el MVP (RTC, buzzer, SD, TWAI) está declarado y `disabled` |
| Fundamento de los datos "verificar" | Razonable y trazable: wiki, demo oficial de Arduino y ESPHome, con un nivel de confianza explícito para cada dato. La polaridad de las DO y DI y el orden RGB del LED están *deducidos*, no medidos. La consola `usb_serial` tiene confianza baja-media (bien señalado) |
| W5500 | Se usa la **INT** (flanco de bajada, `eth_wiznet.c:478`) junto con el monitor de enlace. SPI a 20 MHz (conservador). MAC desde eFuse +3 |
| PSRAM | Declarada (`&psram0` de 8 MB) pero **no habilitada** (`CONFIG_ESP_SPIRAM` desactivado). No hace falta para el MVP, pero la DRAM interna ya está al 54,5 % |
| Segundo núcleo | **No se usa.** `CONFIG_MP_MAX_NUM_CPUS=1`, solo hay objetivo PROCPU y no hay partición para APP_CPU. Es aceptable para la carga del MVP, pero se desaprovecha el núcleo |
| WS2812 | `worldsemi,ws2812-pulse-io` sobre RMT con DMA (`PULSE_IO_ESP32_RMT_DMA=y`): el periférico correcto |
| TCA9554 | Driver dedicado `gpio_tca9554.c` (no `pca95xx`), salidas activas en bajo. Sin glitch al arrancar (ver arriba). La confirmación de las DO es el ACK I²C, no una relectura |
| DI | **Muestreo a 1 kHz** desde un hilo de prioridad 2 (`k_timer`), no por interrupción. Cumple IO-1, pero son 1000 despertares por segundo; con interrupción y temporizador de debounce el coste sería casi nulo |
| Pines de strapping | GPIO0 (BOOT), GPIO3 (CAN RX), GPIO45 (SD D0) y GPIO46 (buzzer): ninguno se maneja en el MVP, así que no hay riesgo de software. GPIO21 (DE) no es de strapping, pero queda flotante hasta `modbus_init_client`. Hay que medir en la placa si el transceptor tiene pull-down |
| RS485 HW frente a GPIO | Se usa la alternativa GPIO. La justificación está verificada en el código. DE se libera en la ISR TX cuando `uart_hal_is_tx_idle()` (`uart_esp32.c:613-618`), es decir, después del bit de stop, lo cual es correcto. Pendiente de medir en la placa: con `TXFIFO_EMPTY` por nivel, la ISR probablemente reentra durante el último carácter (≈1 ms a 9600 baud) hasta que la UART queda inactiva. A 115200 baud el margen frente a los 1,75 ms de giro del esclavo es amplio |

## 6. Calidad del código e idiomatismo

**Idiomatismo de Zephyr (muy alto):**

- DT `zephyr,user` con `di-gpios`/`do-gpios` y `BUILD_ASSERT` sobre la cardinalidad;
- nodo `zephyr,modbus-serial` con `de-gpios`, placa en `BOARD_ROOT`, `Kconfig.gw` con límites (`GW_MAX_*`)
  y `log_config` por módulo;
- `SETTINGS_STATIC_HANDLER_DEFINE` + ZMS, `fstab` LittleFS con automount;
- `conn_mgr` (eventos L4), `task_wdt` con respaldo HW, `k_event`, `k_msgq` y `eventfd` + `zsock_poll`, el
  patrón correcto para el cliente MQTT de bajo nivel;
- `zephyr_nanopb_sources` sobre el proto oficial, con `.options` que eliminan la recursión;
- `generate_inc_file_for_target` para la configuración embebida.

No hay zbus, y tampoco hace falta: el estado coalescido con bits atómicos es más simple y acotado. El
lector JSON propio (370 líneas lógicas) en lugar de `lib/utils/json.c` se justifica por los errores con
línea y columna, pero suma superficie, y es ahí donde está M2.

**Separación lógica/HAL (testabilidad):**

- muy buena en `spb_node` (`struct spb_node_ops` inyectable), `convert`, `config`, `model`, `debounce` y
  `status_led_color`;
- `mb_poller` llama directamente a la API Modbus y `mqtt_link` al cliente, así que la política de
  reintentos, fuera de línea, keepalive y backoff solo se prueba en la e2e.

**Legibilidad:** módulos pequeños con cabeceras que explican el *porqué*, nombres claros, manejo de
errores sistemático y sin `malloc`. `config.c` (655 líneas lógicas) es repetitivo, con un `if/strcmp` por
miembro; una tabla de descriptores lo acortaría. Código muerto mínimo: `MODBUS_FP_EXTENSIONS` queda
activado por defecto y no se usa, y `local_io_di_spec` solo sirve para el shell de simulación.

**Tamaño del código (contado en el repo):**

| Categoría | Líneas | Detalle |
|---|---|---|
| App (`src/*.c` y `*.h`) | **5 001** (≈3 880 sin blancos ni comentarios) | Configuración y JSON: 1 025 lógicas (≈26 %); `spb_node` 590; `mqtt_link` 392; `sparkplug` 277; `mb_poller` 274 |
| Placa y overlays | 513 | DTS, pinctrl, particiones, Kconfig y conf |
| Build y Kconfig | ≈196 | `CMakeLists`, `Kconfig*`, `prj.conf`, `west.yml` |
| Tests unitarios (C) | 1 282 | Más unas 56 de CMake, yaml y conf |
| E2E y scripts (Python) | 562 + 164 | `run_e2e.py`, `modbus_slave.py`, `decode_samples.py`, `mk_config_image.py` |
| Vendorizado | 224 + 34 | `sparkplug_b.proto` (Tahu) y `.options` |
| Generado en el build | ≈197 | `sparkplug_b.pb.{c,h}` (nanopb), no versionado |

**Manifiesto west:** está fijado a la etiqueta `v4.5.0` con `name-allowlist` mínimo, y los módulos quedan
fijados por SHA mediante el `west.yml` de Zephyr. La higiene es correcta. Hay dos detalles:

- se fija por etiqueta y no por SHA;
- se incluyen `mcuboot` y `zcbor`, que no se usan con `ESP_SIMPLE_BOOT`.

Los blobs no hacen falta.

## 7. Puntuación

| Eje | Nota | Justificación |
|---|---|---|
| Completitud MVP | **8** | Todos los IDs están implementados y la e2e lo demuestra. Falta FC15 (MB-4); QA-1 no tiene tests de CRC ni de tramas; nada está validado en la placa |
| Corrección Sparkplug | **9** | bdSeq, seq, LWT, QoS, retain, aliases, orden de BIRTH, rebirth y DCMD son correctos y están verificados con el proto oficial. Solo la configuración incoherente (M2) puede romper la unicidad de los aliases |
| Corrección Modbus | **7** | FC, orden de palabras, escala e inversa, reintentos, excepciones y CRC (subsistema) son correctos. Falta FC15, los equipos caídos saturan el bus (M3) y un tag mudo tumba el dispositivo |
| Robustez / potencial de bugs | **6** | Sin carreras ni desbordamientos, memoria estática. Pero la sesión medio abierta no se detecta (M1, verificado), el validador tiene un agujero (M2) y hay huecos de watchdog y de tormenta de reconexión |
| Aprovechamiento del hardware | **6** | DTS y pinout excelentes, INT del W5500, RMT para el LED, TCA9554 sin glitch. Pero un solo núcleo, sin PSRAM, DI por sondeo, DE por GPIO (justificado) y nada medido en la placa |
| Idiomaticidad del stack | **9** | Uso ejemplar de DT, Kconfig, settings/ZMS, `conn_mgr`, `task_wdt`, eventfd+poll, nanopb y placa HWMv2. El único punto discutible es el parser JSON propio |
| Legibilidad / elegancia | **8** | Código claro, comentado con intención, módulos cohesionados y sin `malloc`. `config.c` es verboso |
| Testabilidad / evidencia | **8** | 32/32 U y 30/30 E reproducidos exactamente, más la verificación con el proto oficial. `mb_poller` y `mqtt_link` no tienen tests unitarios; sin evidencia en hardware |
