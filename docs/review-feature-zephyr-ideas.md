# Revisión de la rama `feature/zephyr-ideas` (ESP-IDF)

- **Rama:** `feature/zephyr-ideas` (`6e6e724`), 7 commits sobre `main` (`d0c9d5b`, que es también la base de fusión). 50 archivos, +1544/−354.
- **Revisor:** independiente; no escribió nada de la rama.
- **Alcance:** los seis puntos que la rama dice resolver (`docs/RECOMMENDATION.md` §4 y §5.2), las regresiones que introduce y la higiene de los commits.
- **Fecha:** 2026-11-03.

## 1. Resumen y veredicto

La rama está bien hecha. Todo lo que dice haber verificado se reproduce: build sin warnings, 68/68 casos de host, decodificación Sparkplug sin errores y **35/35 en el E2E de QEMU**, corrido por el revisor en puertos propios.

Los seis puntos resuelven el problema que atacan, con tests que los cubren:

- la polaridad de las DO con un arranque sin pulsos;
- la configuración embebida usada solo si falta el archivo;
- los errores con ruta JSON o línea y columna, y las claves duplicadas;
- un ciclo de encuesta fallido si falta un bloque;
- los escenarios del broker caído y del broker congelado;
- el motivo de reinicio y el shell.

El port de las ideas de Zephyr es fiel y está bien adaptado a IDF: un `const` struct en lugar del devicetree, `esp_console` en lugar del shell de Zephyr, la misma semántica de ciclo que `mb_poller.c` y la misma política `ENOENT` que `app_config.c`.

Hay un problema que bloquea la fusión:

- **Con la configuración rechazada, las DO no se apagan nunca.** La rama hace que más fallos lleven a esa parada y además documenta lo contrario ("las DO arrancan en OFF siempre"). El TCA9554 no se reinicia con el ESP32. Por eso, una salida que estaba encendida **sigue encendida indefinidamente** tras cualquier reinicio en caliente que termine en una configuración rechazada, sin MQTT para apagarla. El caso típico es regrabar un `config.json` con un error, ya que `esptool` reinicia el chip por EN.

También hay que corregir, porque es la base de un valor por defecto de seguridad, la **cita de la evidencia de la polaridad de las DO**. El README atribuye `inverted: true` a una placa hermana cuyo ESPHome declara los relés con `inverted: false`. La evidencia buena existe, pero es otra.

### Veredicto: **Aprobar con cambios**

Cambios bloqueantes:

1. **(A1)** Llevar las DO a OFF antes de cargar la configuración. `gw_dout_init` no necesita la configuración: se puede separar del muestreo de DI y llamarlo antes de `gw_storage_load_config` (`main.c:113`). Si no, llamarlo al menos en `halt_with_config_error`. Además, corregir README §6.7 y agregar un caso E2E: DO encendida, reinicio con configuración inválida, DO en OFF.
2. **(B1)** Corregir en README §2 la fuente y la confianza de la polaridad de las DO (ver §5.1).

El resto son mejoras no bloqueantes (§4).

## 2. Reproducción

### 2.1 Entorno

- ESP-IDF v6.1 en `/home/user/sdk/idf/esp-idf`.
- Dos árboles de trabajo propios y separados del compartido (`git worktree add --detach`), uno para `6e6e724` y otro para `main`.
- Más árboles para los commits intermedios, cada uno con su directorio de build.
- No se tocó el árbol de trabajo compartido, salvo este archivo.

**Paralelismo.** Las builds de `esp32s3` de HEAD y de `main` corrieron sin límite de trabajos: `idf.py build -- -j2` no pasa `-j` a ninja. El resto (QEMU y commits intermedios) se compiló con `IDF_PY_BUILD_JOBS=2`; los tests de host, con `cmake --build -j2`.

**E2E.** El harness fija los puertos en el código (`e2e.py:37-39`) y en `tools/e2e/fs/config.json:10`. Para la corrida se cambiaron, **solo en la copia del revisor**, a MQTT 28841, Modbus 25030 y consola 25565. El SNTP sigue en 127.0.0.1:123. Solo se detuvieron procesos lanzados por el revisor.

### 2.2 Resultados

| Prueba | `main` (base) | `feature/zephyr-ideas` |
|---|---|---|
| `idf.py set-target esp32s3 build` | OK, 0 warnings, imagen 713 792 B | OK, 0 warnings, imagen 735 072 B (el README dice 734 957; la diferencia es de rutas y versión embebidas) |
| Tests de host (`ctest`) | 9/9 ejecutables, **54/54** casos | 10/10 ejecutables, **68/68** casos |
| `spb_dump` + `spb_decode.py --check` | 5 payloads, 0 errores (NBIRTH de 21 métricas) | 5 payloads, 0 errores (NBIRTH de 22 métricas, `Node Info/Reset Reason` con alias 6) |
| E2E QEMU (`run_e2e.sh`) | no corrido (no se pedía) | **35/35 OK**. Algunas mediciones: broker caído con backoff de 1000, 1964, 3956 y 6997 ms; broker congelado detectado a los 10,0 s (`No PING_RESP`); DDEATH a los 3,04 s; DI→NDATA en 25 ms; `Reset Reason` `power-on` y luego `software` |

### 2.3 Pruebas propias del revisor

**Migración de sdkconfig.** Se reconfiguró la rama con el `sdkconfig` de `main`, al que se le cambió `CONFIG_GW_LED_GPIO=48`:

- `reconfigure` terminó con exit 0 y sin ningún aviso;
- las 17 opciones de pines eliminadas desaparecieron;
- apareció `CONFIG_GW_DO_ACTIVE_LOW=y` (ver B2).

**Parser de configuración** (`probe.c`, enlazado contra `libgw_core.a` del host), con el documento máximo del esquema (16 × 16 tags, 45 883 B):

- pico del árbol cJSON: 223 270 B en el host de 64 bits;
- 7960 asignaciones, de ellas 2928 nodos;
- estimado en 32 bits: unos 157 KB más la sobrecarga del heap (unos 220 KB);
- parseo y validación: 1,0 ms en el host.

**Mensajes de error comprobados:**

- `devices[0].tags[0].scal: clave desconocida`;
- `mqtt.Host: clave desconocida` (las claves distinguen mayúsculas);
- contenido sobrante con línea y columna;
- un BOM UTF-8 se acepta.

**Tests nuevos de `d4001d7` contra el código anterior.** Se compilaron `test_poll.c` y `support.c/h` de `d4001d7` contra el `gw_poll.c` de `b15e0d0`. Falla `test_unanswered_block_fails_the_cycle` (`Expected 2 Was 0`) y **pasa** `test_unanswered_block_skips_the_rest_of_the_cycle`. Ver B3.

**Parada por configuración en QEMU.** En los logs de los arranques con configuración rechazada (`uart0-syntax-error.log`, `uart0-oversized.log`) no aparece ninguna línea de `io:`. `gw_io_init` no corre y la secuencia de DO en OFF nunca se ejecuta (A1).

### 2.4 Commits intermedios

| Commit | Tests de host | Build `esp32s3` |
|---|---|---|
| `5827c15` (BSP y polaridad) | 10/10 ejecutables, 60 casos OK | OK, 0 warnings |
| `7cd1f54` (respaldo solo si falta el archivo) | 10/10, 62 OK | OK, 0 warnings |
| `b15e0d0` (rutas, línea y columna) | 10/10, 65 OK | — |
| `d4001d7` (ciclo fallido) | 10/10, 67 OK | — |
| `31e2d0b` (E2E del broker) | 10/10, 67 OK | — |
| `c4fde60` (REL-5) | 10/10, 67 OK | OK, 0 warnings |
| `6e6e724` (HEAD) | 10/10, 68 OK | OK, 0 warnings |

## 3. Evaluación por punto

| # | Punto | ¿Resuelve el problema? | ¿Correcto? | ¿IDF idiomático? | ¿Fiel a Zephyr? |
|---|---|---|---|---|---|
| 1 | Placa declarativa (`gw_bsp`) y polaridad de las DO | **Sí.** Pines, direcciones y niveles en un solo `const gw_bsp_t`; los drivers no tienen datos de la placa. La secuencia carga `OUTPUT` = OFF y después `CONFIG` = 0 (`gw_dout.c:11-13`), en lógica pura con 6 tests | Sí en arranque en frío y en reinicio en caliente con configuración válida. **No en la parada por configuración** (A1). La evidencia está mal citada (B1) | Sí. IDF no tiene devicetree y un struct `const` con máscaras de polaridad es razonable (esp-bsp usa macros y funciones de init; esto es equivalente). Solo quedan en Kconfig las dos polaridades inciertas | Sí: equivale a `do-gpios = <&exio n GPIO_ACTIVE_LOW>` del DTS de Zephyr, con la conversión explícita en `gw_dout` |
| 2 | Configuración a prueba de fallos | **Sí** (H1 de la review de IDF). Solo `ENOENT` elige la embebida (`gw_storage.c:128-139`). Montaje, lectura, tamaño, vacío, falta de memoria e inválida detienen el equipo. Lo confirma el E2E en 6 casos | Sí. Búfer y árbol en PSRAM con `heap_caps_malloc_prefer` y retorno a la RAM interna; la falta de memoria se informa como tal y el búfer se libera en todos los caminos. Pero la parada deja las DO como estaban (A1) | Sí (`heap_caps_*`, Kconfig por componente) | Sí: la misma política que `app_config.c` (solo `-ENOENT` usa el default) y un límite configurable (Zephyr tiene `GW_CONFIG_MAX_SIZE`) |
| 3 | Errores con ruta JSON y línea:columna; claves duplicadas | **Sí.** Ruta de la clave (`devices[0].tags[0].scal`), línea y columna contadas en caracteres, contenido sobrante y claves duplicadas en los 9 tipos de objeto (todos pasan por `check_keys`) | Sí. La búsqueda de duplicados es O(k²) con k ≤ 10 claves. Detalle menor: un `\0` dentro del archivo seguido de basura se acepta (`gw_config.c:582`) | Sí | Sí: corrige además el hallazgo Media de Zephyr (duplicados aceptados) |
| 4 | Semántica del ciclo de encuesta | **Sí** (M1). Un bloque sin respuesta tras los reintentos hace fallar el ciclo, los tags no leídos conservan su valor y su timestamp (no se reportan como frescos), `offline_after` ciclos llevan al DDEATH y solo un ciclo completo devuelve el equipo a en línea | Sí. Los tags no leídos siguen con `valid = true` hasta el DDEATH (como mucho `offline_after` ciclos); es la semántica elegida y está documentada | Sí | **Muy fiel**: es la de `poll_device()` en `mb_poller.c:191-243` (corta al primer fallo; la excepción cuenta como respuesta) |
| 5 | E2E: reinicio del broker y broker congelado | **Sí.** Los dos pasan, con NDEATH (will), bdSeq nuevo, NBIRTH con seq 0 y todos los DBIRTH | Sí. Defectos menores del harness: puertos fijos, límite fijo y comprobaciones negativas basadas en el log (B4) | — | Sí, y amplía el escenario de Zephyr con la verificación de los DBIRTH y de los will tardíos |
| 6 | Motivo de reinicio en NBIRTH y shell de diagnóstico | **Sí.** `Node Info/Reset Reason` (String, alias 6, sin colisión: el mapa usa 1–5, 10–17, 20–27 y 100+). Shell `esp_console` con `status`, `devices` y `config`, que también funciona con la configuración rechazada. Contadores por dispositivo (MB-13) | Sí. El shell solo lee a través de `gw_tagdb` (bajo mutex) y de `gw_status_get` (atómico); `s_cfg` es inmutable. La contraseña no se imprime | Sí: `esp_console_new_repl_uart` o `_usb_serial_jtag` según el Kconfig de la consola, y el comando `help` | REL-5 va más allá que Zephyr (que solo hace `LOG` de `hwinfo_get_reset_cause`). El shell es un superconjunto de `gw status` |

### 3.1 ¿Puede el arranque encender una DO? (punto 1)

Se analizó con DO activa en bajo, el valor por defecto.

**Arranque en frío.** El TCA9554 arranca con `OUTPUT` = 0xFF y `CONFIG` = 0xFF: todos los pines son entradas con pull-up de unos 100 kΩ, en alto, es decir, en OFF. `gw_dout_init` escribe primero `OUTPUT` = 0xFF (no cambia nada) y luego `CONFIG` = 0x00: los pines pasan a salida en alto. **No hay pulso.**

**Reinicio en caliente** (`esp_restart`, NCMD Reboot, panic, TWDT/IWDT, reset por EN durante `idf.py flash`). El TCA9554 no tiene pin de reset y se alimenta de 3,3 V, así que conserva `OUTPUT` y `CONFIG`.

- No se enciende ninguna salida apagada: no hay pulso.
- Las salidas que estaban encendidas **siguen encendidas** durante el bootloader, la carga de la configuración y `gw_edge_start`, hasta `gw_io_init` (`main.c:125`). Después se apagan.
- **Si la configuración se rechaza (`main.c:113-114`), ese momento no llega nunca:** la salida queda encendida mientras el equipo dice "nothing is started" (A1).

**Brownout del ESP32.** Si el TCA9554 no llega a su umbral de POR, el caso es igual al reinicio en caliente.

**Polaridad configurada en alto** (`CONFIG_GW_DO_ACTIVE_LOW=n`). Entre el POR y la inicialización, los pines son entradas con pull-up en alto, que en ese caso es ON. Pero 100 kΩ no alcanzan para encender un optoacoplador; después se escribe `OUTPUT` = 0x00 antes de `CONFIG`.

**Conclusión.** La secuencia `gw_dout` es correcta y nunca genera un flanco a ON. El problema es la falta de esa secuencia en los caminos de parada.

## 4. Hallazgos (por severidad)

| ID | Sev. | Dónde | Hallazgo y escenario |
|---|---|---|---|
| **A1** | **Alta** | `idf/main/main.c:113-114` (parada) frente a `:125` (`gw_io_init`); `idf/README.md:394` | **Las DO no se apagan si la configuración se rechaza, y el README garantiza lo contrario.** La rama agrega casos que paran el arranque: LittleFS sin montar, lectura, tamaño, vacío y falta de memoria. En `main` esos casos arrancaban con la embebida y llegaban a `gw_io_init`. El README §6.7 dice ahora "Las DO arrancan en OFF siempre, también tras un reset por watchdog". **Escenario:** DO/3 encendida por NCMD (por ejemplo, una válvula). El técnico regraba con `idf.py flash` un `config.json` con una coma de más (el procedimiento de §3.3). `esptool` reinicia el ESP32 por EN (`--after hard-reset`), pero el TCA9554 no se reinicia y conserva el latch; la configuración se rechaza y el equipo queda "detenido" con el LED rojo. **DO/3 sigue energizada indefinidamente**, sin MQTT para apagarla. Lo mismo pasa con un panic o un TWDT seguido de un error de lectura de LittleFS o de falta de memoria. Comprobado en QEMU: en los arranques rechazados no corre ninguna línea de `io:`. El caso de sintaxis ya existía en `main`; la rama lo amplía y lo documenta al revés. **Solución:** separar de `gw_io_init` la inicialización de las DO (`gw_dout_init` no depende de `cfg`) y llamarla antes de `gw_storage_load_config`; agregar un test E2E |
| **B1** | Baja (documentación, pero sobre un valor por defecto de seguridad) | `idf/README.md:102`, `:109`; mensaje de `5827c15` | **La evidencia de la polaridad de las DO está mal citada.** El README cita como fuente el ESPHome "de la placa hermana 8DI-8RO" y le atribuye `inverted: true` en las 8 DO. Esa definición (`esphome8ro.md` en el scratchpad) tiene `inverted: true` **solo en las DI y en el botón BOOT; los relés son `inverted: false`**. La fuente que sí lo dice es otra: la configuración de ESPHome de la propia `Waveshare-ESP32-S3-ETH-8DI-8DO` (`esphome-devices`, `config.yaml`), con `DO1..DO8 inverted: true`, que el revisor descargó y comprobó. El argumento del demo es válido: `DIN_Inverse_Enable = 0` y el modo DI→DO copia el nivel crudo. Pero hay **contraevidencia** que el README no menciona: el demo llama `Dout_Open()` al nivel ALTO, `"CH1":1` por MQTT pone el pin en alto, y el wiki (copiado del de relés) dice "ALL_ON … high level (on state)". **Confianza real: Media**, no "Media-alta". El valor por defecto (activa en bajo) es el más probable, pero medir antes de conectar cargas es obligatorio |
| B2 | Baja | `components/gw_board/Kconfig`, `gw_net/Kconfig`, `gw_rs485/Kconfig` (opciones eliminadas); `gw_bsp/Kconfig` (`GW_DO_ACTIVE_LOW`) | **Un `sdkconfig` existente cambia en silencio.** Comprobado: con el `sdkconfig` de `main` y `CONFIG_GW_LED_GPIO=48`, `reconfigure` termina sin ningún aviso, se descartan las 17 opciones de pines (6 de `gw_board`, 7 de `gw_net` y 4 de `gw_rs485`) y aparece `CONFIG_GW_DO_ACTIVE_LOW=y`, que **invierte la polaridad de las DO** de una build ya probada. No rompe la build. Como no hay equipos desplegados, alcanza con una nota de migración en el README |
| B3 | Baja | Mensaje de `d4001d7` | Dice que los dos tests nuevos "fallan contra el código anterior". Solo falla `test_unanswered_block_fails_the_cycle`; `test_unanswered_block_skips_the_rest_of_the_cycle` pasa también contra `b15e0d0`, porque el `break` anterior ya cortaba el ciclo |
| B4 | Baja | `tools/e2e/e2e.py:37-39`, `:115-118`, `:405`, `:427`; `tools/e2e/fs/config.json:10` | **Calidad del harness:** puertos MQTT, Modbus y consola fijos en el código y en el JSON, y SNTP en 127.0.0.1:123. Dos corridas en paralelo, de CI o de dos personas, chocan. El límite `128 * 1024` está fijo en lugar de leerse de `sdkconfig`. La comprobación "no arrancó nada" busca tres textos del log (`connecting to\|rs485: UART\|net: IPv4`): si se cambia uno, pasa sin comprobar nada. El `status` del shell con la configuración **válida** no se prueba. **Solución:** puertos por argumento o variable de entorno, y una señal positiva de "parado" (por ejemplo, la línea `nothing is started`) |
| B5 | Baja | `components/gw_board/gw_io.c:160-166` | **El NBIRTH puede informar mal las DO.** Si `gw_dout_init` falla tras un reinicio en caliente (TCA9554 que no responde por un instante), no se llama a `publish_do()`, `gw_tagdb` queda en `dout = 0` y el NBIRTH publica `DO/n = false` aunque el latch físico siga con salidas encendidas. Ya pasaba en `main` |
| B6 | Baja | `components/gw_core/src/gw_dout.c:5-17` | **El registro de polaridad no se reinicia.** `gw_dout_init` no escribe `POLARITY` (0x02) = 0. Si un firmware anterior, sin cortar la alimentación, lo dejó distinto de 0, la relectura de las DO sale invertida. Es una línea de robustez |
| B7 | Baja | `components/gw_storage/gw_storage.c:69-72`; `sdkconfig.defaults` (`SPIRAM_IGNORE_NOTFOUND`) | **Sin PSRAM, una configuración grande detiene el equipo.** En QEMU, o en una placa donde la PSRAM no se detecte (está activo `IGNORE_NOTFOUND`), el documento máximo necesita unos 160–220 KB de árbol más el archivo, en RAM interna. El equipo se detiene con "memoria insuficiente". Es a prueba de fallos y está documentado de forma genérica ("el límite real es el heap interno libre"). Convendría decir en el README qué tamaño entra sin PSRAM |
| B8 | Baja | Commit `6e6e724` | Mezcla dos cambios: el shell (CFG-6) y los contadores por dispositivo del motor de encuesta (MB-13, `gw_poll.c`, `gw_tagdb`). Debieron ser dos commits; el mensaje lo dice |

### 4.1 Lo que se revisó y no tiene problemas

- **Concurrencia del shell.** `gw_tagdb_get_device`, `_get_stats` y `_get_io` copian bajo `gw_port_lock`; `gw_status_get` usa `atomic_load`; `s_cfg` y `s_info` son inmutables tras el arranque. El búfer `s_vals` del shell solo lo usa la tarea del REPL.
- **Watchdog.**
  - El REPL (prioridad 2, 4 KB) no está suscrito al TWDT, pero bloquea en la UART y no puede dejar sin CPU a las tareas IDLE.
  - El parseo del documento máximo tarda 1 ms en el host. En el S3 con PSRAM son decenas de ms, lejos de los 15 s del TWDT. `app_main` no está suscrita.
- **Sparkplug.**
  - Agregar una métrica al NBIRTH es compatible con los hosts.
  - El alias 6 estaba libre.
  - `GW_NODE_BIRTH_METRICS` (22) dimensiona el arreglo de métricas.
  - El NBIRTH ocupa 605 B, frente a un búfer de 6 KB.
  - Los 36 mensajes del E2E se decodifican con el proto oficial.
- **Ganchos de cJSON.** Son globales y quedan apuntando a `boot_alloc`. Nadie más usa cJSON tras el arranque, y `heap_caps_free` es válido para los dos tipos de memoria.
- **Caminos de error de la carga.** El búfer se libera siempre. Ante un fallo de memoria, cJSON libera el árbol parcial. `errno` se captura después de `fread`.

### 4.2 Fuera del alcance de la rama, todavía pendientes

Son de `RECOMMENDATION.md` §4.2 y review-idf:

- **FC16 a baudios bajos:** `uart_wait_tx_done` con 100 ms fijos (`gw_rs485.c:36`, M2).
- **Bus saturado** por los dispositivos fuera de línea.
- **TWDT durante un rebirth** sobre un enlace lento.
- `sdkconfig.defaults` asigna `ESP_COREDUMP_DATA_FORMAT_ELF`, que IDF 6.1 no reconoce (aviso `unknown kconfig symbol` al reconfigurar). Ya estaba en `main`: probablemente el formato del coredump no es el que se quiso configurar.

## 5. Notas por punto

### 5.1 Evidencia de la polaridad de las DO (revisada de forma independiente)

| Fuente | Qué dice | Peso |
|---|---|---|
| Demo `WS_Dout.cpp`: `Dout_Init()` → `TCA9554PWR_Init(0x00, 0xFF)` | El latch arranca en 0xFF, que es también el valor de reset del chip. Encaja con "alto = OFF", pero el demo podría simplemente no tocar nada | Débil |
| Demo `WS_DIN.cpp`/`.h`: `DIN_Inverse_Enable = 0`, `Dout_Immediate_CHxs(DIN_Data)` | Copia el nivel crudo de la DI (reposo = 1 por el pull-up) a la DO. Para que "DI activa → DO activa" tenga sentido, nivel bajo tiene que ser DO activa | Media |
| Demo `WS_Dout.cpp`/`WS_MQTT.cpp`: `Dout_Open()` = `Set_EXIO(…, true)` (alto); `"CH1":1` → alto; `Dout_Flag = {1,…}`; botones de la web "All High" y "All Low" | La nomenclatura del demo dice "Open" para el nivel alto. Si "Open" significa "encender", es **contraevidencia**. Que la web hable de niveles y no de encendido sugiere que los autores evitaron la ambigüedad | Ambigua |
| Wiki del 8DO (`wiki.txt:367`) | "ALL_ON … high level (on state)", pero el texto está copiado del de relés (`Relay_Analysis`, `Relay_Flag`) | Débil, en contra |
| ESPHome `Waveshare-ESP32-S3-ETH-8DI-8RO` (la placa hermana que cita el README) | Relés con `inverted: false` (alto = ON). Otro circuito: relés, no Darlington con opto | No aplica, y contradice la cita del README |
| ESPHome `Waveshare-ESP32-S3-ETH-8DI-8DO` (`config.yaml`, comunitario, publicado en 2025-10) | `DO1..DO8` con `inverted: true` | Media-alta: la fuente más directa |
| Zephyr (`esp32s3_poe_eth_8di_8do_procpu.dts:70-73`) | `GPIO_ACTIVE_LOW`, a partir de las mismas fuentes | No es independiente |

**Juicio.** Activa en bajo es la hipótesis más probable y el valor por defecto correcto. Con un esquemático o una medición se resolvería. La confianza es **Media**. El requisito de medir antes de cargar debe seguir en negrita.

### 5.2 Semántica del ciclo (punto 4)

`gw_poll.c:101-131` reproduce el `poll_device()` de Zephyr: la excepción cuenta como respuesta, y el ciclo se corta en el primer bloque sin respuesta, lo que además ahorra los timeouts de los demás.

Consecuencia deliberada, que conviene destacar en el README de puesta en marcha: un esclavo que **nunca** contesta un bloque, como un mapa de registros mal configurado que da timeout en lugar de excepción, **no pasa nunca a en línea**. Con `main`, ese equipo aparecía en línea y con datos parciales.

### 5.3 Configuración (puntos 2 y 3)

Están bien resueltos.

- El límite de 128 KiB cubre con holgura el máximo del esquema: 46 KB compacto, 82 KB con sangría.
- El tamaño se rechaza antes de asignar memoria.
- `stat` distingue `ENOENT` de otros `errno`.
- El E2E cubre seis arranques: demasiado grande, sin montar, vacío, sintaxis, esquema y ausente.

## 6. Higiene de los commits

| Commit | ¿Enfocado? | ¿Compila y pasa sus tests solo? | ¿El mensaje coincide? |
|---|---|---|---|
| `5827c15` BSP y polaridad | Sí. Grande (23 archivos), pero es un solo cambio: los pines a `gw_bsp`, más `gw_dout` | Sí (build 0 warnings; 60 casos) | Sí, salvo la cita de ESPHome (B1) |
| `7cd1f54` respaldo solo si falta el archivo | Sí. Incluye la subida del límite y el asignador PSRAM, que son parte del mismo hallazgo H1 | Sí (build 0 warnings; 62 casos) | Sí |
| `b15e0d0` rutas, línea y columna | Sí | Sí (65 casos) | Sí |
| `d4001d7` ciclo fallido | Sí | Sí (67 casos) | Casi: solo uno de los dos tests falla con el código anterior (B3) |
| `31e2d0b` E2E del broker | Sí (solo el harness y el README) | Sí (67 casos; el E2E solo en HEAD) | Sí |
| `c4fde60` REL-5 | Sí. Agrupa las cadenas en `gw_node_info_t`, un refactor que necesita | Sí (build 0 warnings; 67 casos) | Sí |
| `6e6e724` shell | **No del todo:** shell más contadores MB-13 (B8) | Sí (build 0 warnings; 68 casos; E2E 35/35) | Sí, lo explica. "Imagen 734957 B" depende del entorno |

Cada commit actualiza el README con su cambio y su evidencia. Los mensajes explican el porqué y nombran el hallazgo que cierran (H1, M1, CFG-6, REL-5). Las líneas de atribución son correctas.

## 7. Veredicto

**Aprobar con cambios.** Bloqueantes:

1. **A1:** DO en OFF antes de cargar la configuración (o en la parada), con un test, y el README §6.7 corregido.
2. **B1:** corregir la fuente y la confianza de la polaridad de las DO en el README §2.

Recomendados, no bloqueantes: B2 (nota de migración del `sdkconfig`), B3 (corregir el mensaje si se reescribe la historia), B4 (puertos del harness y una señal positiva de "parado"), B5 y B6 (robustez de las DO), B7 (tamaño máximo sin PSRAM) y B8.
