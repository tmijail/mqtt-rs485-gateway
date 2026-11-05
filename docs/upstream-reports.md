# Reportes para proyectos de terceros (pendientes de enviar)

Defectos de componentes de Espressif (incluido su QEMU) encontrados al desarrollar el firmware. Desde el entorno de desarrollo no se pueden abrir issues en esos repositorios: el texto está listo para que el mantenedor lo envíe. Cada uno dice dónde está el arreglo provisorio en este repositorio, para quitarlo cuando haya una versión corregida, y si ya hay un issue parecido arriba.

## 1. espressif/esp-mqtt: `esp_mqtt_set_config()` pierde memoria con `network.if_name` y `key_password`

- **Repositorio:** https://github.com/espressif/esp-mqtt (componente `espressif/mqtt`).
- **Versión:** 1.1.0 (la fijada en `idf/components/gw_edge/idf_component.yml`).
- **Arreglo provisorio aquí:** `set_config()` en `idf/components/gw_edge/gw_edge.c` y el test `check_set_config` (issue #58).

Texto propuesto (en inglés):

> **Title:** esp_mqtt_set_config() leaks network.if_name and authentication.key_password on every call
>
> **Version:** espressif/mqtt 1.1.0 (IDF v6.1)
>
> `esp_mqtt_set_config()` allocates a new copy of `config->network.if_name` (mqtt_client.c, around line 633: `client->config->if_name = calloc(1, sizeof(struct ifreq) + 1)`) without freeing the previous `client->config->if_name`. The same happens with `credentials.authentication.key_password` (`client->config->clientkey_password = malloc(...)`, around line 683). Every call with either field set leaks one allocation (about 17 bytes for `if_name`).
>
> Applications that call `esp_mqtt_set_config()` before every reconnection, for example to refresh the last will (Sparkplug B puts a new bdSeq in the will of every connection attempt), leak continuously.
>
> **Steps:** create a client with `network.if_name` set, then call `esp_mqtt_set_config()` with the same configuration in a loop; the free heap goes down by the size of `struct ifreq` + 1 per call.
>
> **Expected:** free the previous copy before allocating the new one (as done for the host, the client id and the will message), or keep the existing allocation.

## 2. ESP-IDF esp-tls (mbedTLS): con TLS 1.3 no se informa por qué falló la verificación del certificado

- **Repositorio:** https://github.com/espressif/esp-idf, componente `esp-tls` (`components/esp-tls/esp_tls_mbedtls.c`).
- **Versión:** ESP-IDF v6.1, mbedTLS 4.1.
- **Arreglo provisorio aquí:** `idf/components/gw_edge/gw_edge.c` registra los *flags* con su propio callback de verificación de mbedTLS (issue #70).

Texto propuesto (en inglés):

> **Title:** esp-tls does not report the certificate verification flags when a TLS 1.3 handshake fails
>
> **Version:** ESP-IDF v6.1 (mbedTLS 4.1), client side
>
> When the server certificate fails verification, `esp_mbedtls_handshake()` calls `esp_mbedtls_verify_certificate()` only if `mbedtls_ssl_get_peer_cert()` returns a certificate (esp_tls_mbedtls.c, around line 291). With TLS 1.3 the peer certificate is no longer available after the failed handshake, so `ESP_TLS_ERR_TYPE_MBEDTLS_CERT_FLAGS` is never captured: `esp_tls_get_and_clear_last_error()` (and esp-mqtt's `esp_tls_cert_verify_flags`) report 0, and the application only sees `MBEDTLS_ERR_X509_CERT_VERIFY_FAILED` (-0x2700) without knowing whether the CA, the name or the validity dates were wrong. (Observed with TLS 1.3; the code path does not depend on the version, so TLS 1.2 is likely affected whenever the peer certificate is freed on failure.)
>
> **Steps:** connect with esp-tls (for example through esp-mqtt) to a TLS 1.3-only server whose certificate is expired, or carries another name; read the error handle after the failure.
>
> **Expected:** the verification flags of the failed handshake, for example by calling `mbedtls_ssl_get_verify_result()` regardless of the peer certificate, or by recording them in a verify callback set by esp-tls.

## Proyecto de reproducción para espressif/qemu (secciones 3 a 5)

Los tres reportes de QEMU (issue #82) usan la misma aplicación mínima de ESP-IDF, sin nada de este repositorio: se reprodujo con ella cada defecto que en el gateway estaba arreglado de forma provisoria. Va dentro de cada reporte (o una vez, enlazada desde los otros). La aplicación encadena los casos por la causa del reinicio: en el arranque normal registra `esp_timer_get_time()` y llama a `esp_restart()`; en el siguiente (reinicio por software) lo vuelve a registrar, deja una tarea en el núcleo 0 que registra y crea tareas cortas, y hace `abort()` en el núcleo 1 con el volcado de memoria a flash; en el tercero (pánico) IDF verifica ese volcado.

Medido el 2026-11-04 con `QEMU emulator version 9.2.2 (esp_develop_9.2.2_20260417)` (la que instala `idf_tools.py` para ESP-IDF v6.1). Resultados en 5 corridas: el núcleo 0 siguió registrando entre 94 y 117 líneas mientras el núcleo 1 escribía el volcado; los 5 volcados quedaron inválidos (5 `abort()` anidados en `core_dump_flash.c:158`); el temporizador nunca volvió a 0. Con `is_octal` QEMU se cayó 3 de 3 veces.

Texto común (en inglés):

> **Reproduction project** (ESP-IDF v6.1, target esp32s3, nothing else needed)
>
> `main/main.c`:
>
> ```c
> #include <stdio.h>
> #include <inttypes.h>
> #include "freertos/FreeRTOS.h"
> #include "freertos/task.h"
> #include "esp_timer.h"
> #include "esp_system.h"
> #include "esp_log.h"
>
> static void short_lived(void *arg)
> {
>     vTaskDelay((uintptr_t)arg);
>     vTaskDelete(NULL);
> }
>
> static void busy_core0(void *arg)
> {
>     for (unsigned i = 0;; i++) {
>         ESP_LOGW("core0", "still running on core %d (%u)", xPortGetCoreID(), i);
>         for (uintptr_t t = 1; t <= 3; t++) {   /* the task list keeps changing */
>             xTaskCreatePinnedToCore(short_lived, "tmp", 2048, (void *)t, 5, NULL, 0);
>         }
>         vTaskDelay(1);
>     }
> }
>
> static void crash_core1(void *arg)
> {
>     vTaskDelay(pdMS_TO_TICKS(500));
>     printf("abort() on core %d\n", xPortGetCoreID());
>     abort();
> }
>
> void app_main(void)
> {
>     esp_reset_reason_t r = esp_reset_reason();
>     printf("REPRO reset reason %d, esp_timer_get_time() at app_main: %" PRId64 " us\n", (int)r,
>            esp_timer_get_time());
>     if (r == ESP_RST_POWERON) {            /* boot 1 */
>         vTaskDelay(pdMS_TO_TICKS(3000));
>         printf("REPRO esp_restart() at %" PRId64 " us\n", esp_timer_get_time());
>         esp_restart();
>     }
>     if (r == ESP_RST_SW) {                 /* boot 2 */
>         xTaskCreatePinnedToCore(busy_core0, "busy", 3072, NULL, 5, NULL, 0);
>         xTaskCreatePinnedToCore(crash_core1, "crash", 3072, NULL, 10, NULL, 1);
>         return;
>     }
>     printf("REPRO done (reset reason %d)\n", (int)r);   /* boot 3: after the panic */
> }
> ```
>
> `main/CMakeLists.txt`: `idf_component_register(SRCS "main.c" PRIV_REQUIRES esp_timer esp_system)`
>
> `partitions.csv`:
>
> ```
> nvs,      data, nvs,      0x9000,  0x6000
> otadata,  data, ota,      0xf000,  0x2000
> phy_init, data, phy,      0x11000, 0x1000
> ota_0,    app,  ota_0,    0x20000, 0x100000
> coredump, data, coredump, ,        0x10000
> ```
>
> `sdkconfig.defaults`:
>
> ```
> CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
> CONFIG_PARTITION_TABLE_CUSTOM=y
> CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
> CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y
> CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y
> ```
>
> Build and run (a fresh image every time):
>
> ```
> idf.py set-target esp32s3 build
> (cd build && esptool.py --chip esp32s3 merge-bin --pad-to-size 4MB -o ../flash.bin @flash_args)
> qemu-system-xtensa -M esp32s3 -m 8M -nographic -drive file=flash.bin,if=mtd,format=raw
> ```
>
> QEMU: `QEMU emulator version 9.2.2 (esp_develop_9.2.2_20260417)`, x86-64 Linux host.

## 3. espressif/qemu: un pánico no detiene el otro núcleo (volcado de memoria corrupto)

- **Repositorio:** https://github.com/espressif/qemu
- **Versión:** QEMU 9.2.2 `esp_develop_9.2.2_20260417`, máquina `esp32s3`, ESP-IDF v6.1.
- **Ya reportado arriba (la causa):** [espressif/qemu#189](https://github.com/espressif/qemu/issues/189) (abierto: "a restart from the APP CPU boots the PRO CPU two or three times (CPU stall not modelled, CPU reset is a machine reset)") explica que `esp32s3_cpu_stall()` está vacía, con el arreglo en el PR [espressif/qemu#190](https://github.com/espressif/qemu/pull/190) (abierto). Ese issue muestra el síntoma en el reinicio; no menciona el volcado de memoria. **Conviene enviar este texto como comentario en #189** (otro síntoma de la misma causa, que sirve para probar el PR), no como un issue nuevo.
- **Arreglo provisorio aquí:** `cmd_panic()` en `idf/components/gw_shell/gw_shell.c` (issue #62): el comando `panic` del emulador estaciona el otro núcleo (`esp_ipc_call` con las interrupciones apagadas) antes de `abort()`, como haría el hardware. Sin eso, 2 o 3 de cada 3 volcados del E2E salían inválidos. El check del E2E (issue #88) exige además que el propio equipo dé el volcado por válido al arrancar, y distingue el síntoma del emulador (otra tarea escribiendo en el log durante el volcado) de un volcado que el firmware escribió mal.

Texto propuesto (en inglés, como comentario en #189):

> **Another symptom of the missing CPU stall: core dumps to flash are corrupted**
>
> **Version:** QEMU 9.2.2 (esp_develop_9.2.2_20260417), `-M esp32s3`, ESP-IDF v6.1
>
> ESP-IDF's panic handler stalls the other core before it writes the core dump (`esp_cpu_stall()` in `components/esp_system/panic_handler.c`, an RTC_CNTL software stall). On `-M esp32s3` the other core keeps running: its tasks keep logging, switching and changing stacks and the task list between the two passes of the ELF core dump writer (the first pass computes the size, the second writes). The dump overflows its computed size, the writer asserts inside the panic handler and the dump left in flash fails its checksum.
>
> **Steps:** the reproduction project above (core 0 keeps logging and creating short-lived tasks; core 1 calls `abort()` with the core dump to flash enabled).
>
> **Expected:** as on silicon, nothing runs on core 0 after the panic on core 1; the core dump is saved and is valid at the next boot.
>
> **Observed** (5 runs out of 5): core 0 logged between 94 and 117 lines after the panic started; every dump was invalid, and 5 of 5 runs aborted again inside the core dump writer (`esp_core_dump_flash_write_data` at `components/espcoredump/src/core_dump_flash.c:158`, called from `elf_process_task_stack`):
>
> ```
> abort() on core 1
> W (2518) core0: still running on core 0 (52)
> ...
> abort() was called at PC 0x4200b0c7 on core 1
> ...
> I (1107) esp_core_dump_flash: Save core dump to flash...
> I (1108) esp_core_dump_flash: Erase flash 20480 bytes @ 0x120000
> W (2728) core0: still running on core 0 (73)
> W (2738) core0: still running on core 0 (74)
> ...
> abort() was called at PC 0x4200a763 on core 1
> ...
> Re-entered core dump! Exception happened during core dump!
> Rebooting...
> ...
> E (5109) esp_core_dump_flash: Core dump data check failed:
> Calculated checksum='6b8c87f8437de8f22c8129d5c8f5b11a622d4503a3722bdc4eb321aabadb66c0'
> Image checksum='2c6b3740e05905403009060051aa3780d0ebce3f230906000000000020090600'
> ```
>
> Core 0's log lines also get interleaved into core 1's panic backtrace. In a larger application we also saw `Invalid core dump SHA256` and `stream read less than specified amount` when reading the dump back. Parking core 0 from the application before the `abort()` (a busy loop with interrupts disabled, entered through `esp_ipc_call`) makes every dump valid, which points at the stall. A `xtensa_runstall()`-based `esp32s3_cpu_stall()`, as in PR #190, should fix this too.
>
> The same happens under host CPU load, with another symptom: in a gateway firmware that aborts from its console task (nothing parked), with two busy loops on a 4-CPU host, 1 dump in 6 was left without its final checksum although the writer logged `Write end offset 0x91e0, check sum length 32` and `Core dump has been saved to flash.`. The device itself then reads the checksum back as the erased pattern (`Core dump data check failed`; `esp_coredump`: `Invalid core dump SHA256 "b94e…", should be "a5a5…a5"`), and a task of the other core logged inside the dump window (`W (7398) buzzer: alarm …`, between `Save core dump to flash...` and the restart). Our guess, not verified in the QEMU sources: the running core's flash-cache fetches interleave with the SPI1 page program of the panic handler, which on silicon cannot happen because that core is stalled. With the other core parked, 21 of 21 dumps under the same load were valid.

## 4. espressif/qemu: con `ssi_psram` en modo octal, QEMU se cae (SIGSEGV en `psram_quad_read`) cuando el bootloader escribe la flash

- **Repositorio:** https://github.com/espressif/qemu
- **Versión:** QEMU 9.2.2 `esp_develop_9.2.2_20260417`, máquina `esp32s3`, ESP-IDF v6.1.
- **Ya reportado arriba:** no. Los issues sobre la PSRAM octal ([#109](https://github.com/espressif/qemu/issues/109), [#137](https://github.com/espressif/qemu/issues/137), cerrados) trataban de que no se detectaba; se resolvieron con la propiedad `is_octal`, que es la que dispara este defecto. [#175](https://github.com/espressif/qemu/issues/175) (abierto) es otro SIGSEGV (código generado con MTTCG); este también ocurre con `-accel tcg,thread=single`.
- **Arreglo provisorio aquí:** el E2E corre sin PSRAM (`CONFIG_SPIRAM=n` en `idf/sdkconfig.qemu`; README §6, issue #63). La variante `E2E_PSRAM=1 tools/e2e/run_e2e.sh` queda para cuando QEMU lo corrija. Al reducir el caso se encontró la condición exacta: **la escritura de `otadata` que hace el bootloader en el primer arranque** (`Set actual ota_seq=1 in otadata[0]`). Con la imagen del gateway arrancada antes una vez sin `is_octal` (es decir, con el `otadata` ya escrito), arrancó 3 de 3 veces con la PSRAM octal (8 MB detectados, sin caídas). Eso explica lo medido en #63: con imagen nueva se caía casi siempre, y con una ya arrancada, 1 de 6 veces.

Texto propuesto (en inglés):

> **Title:** esp32s3: with `ssi_psram` `is_octal=true`, a SPI1 flash write crashes QEMU (SIGSEGV in psram_quad_read, addr = -1)
>
> **Version:** QEMU 9.2.2 (esp_develop_9.2.2_20260417), `-M esp32s3`, ESP-IDF v6.1; also with `-accel tcg,thread=single`
>
> **Steps:** the reproduction project above (the application does not even need `CONFIG_SPIRAM`; what matters is the partition table with `otadata` and `ota_0` and an erased `otadata`, so that the 2nd stage bootloader writes the first `otadata` entry). Run it with the octal PSRAM model, as `idf.py qemu` does for an octal PSRAM build:
>
> ```
> qemu-system-xtensa -M esp32s3 -m 8M -nographic -drive file=flash.bin,if=mtd,format=raw \
>     -global driver=ssi_psram,property=is_octal,value=true
> ```
>
> **Expected:** the bootloader writes `otadata` ("Set actual ota_seq=1 in otadata[0]") and starts the application, as it does without `is_octal`.
>
> **Observed:** 3 out of 3 runs, the QEMU process dies with SIGSEGV right after `I (94) boot: Loaded app from partition at offset 0x20000`, the line before the bootloader's `otadata` write. Backtrace of the QEMU process:
>
> ```
> #0  psram_quad_read (s=...) at ../hw/misc/ssi_psram.c:154
> #1  psram_transfer (dev=<optimized out>, value=<optimized out>) at ../hw/misc/ssi_psram.c:440
> #2  ssi_transfer (bus=<optimized out>, val=255) at ../hw/ssi/ssi.c:165
> #3  esp32s3_spi_txrx_buffer (s=..., tx=<optimized out>, tx_bytes=32, rx=<optimized out>, rx_bytes=0) at ../hw/ssi/esp32s3_spi.c:148
> #4  esp32s3_spi_perform_transaction (s=..., t=...) at ../hw/ssi/esp32s3_spi.c:196
> #5  esp32s3_spi_special_command (command=<optimized out>, s=<optimized out>) at ../hw/ssi/esp32s3_spi.c:368
> #6  esp32s3_spi_write (opaque=<optimized out>, addr=0, ...) at ../hw/ssi/esp32s3_spi.c:387
> ```
>
> In frame 0, `s->addr == -1`, `s->command == 0` and `s->state == ST_PROCESSING`: the 32-byte page program of the `otadata` entry (an SPI1 transaction for the flash) reaches the PSRAM model, which has not been through a command/address phase, and it reads its buffer at offset -1. Without `is_octal` the same image boots; once `otadata` has been written (for example by one boot without `is_octal`), later boots with `is_octal` work, PSRAM included. Two problems, then: the flash transaction should not reach the PSRAM device (CS1), and `psram_quad_read`/`psram_transfer` should not index the buffer without a valid address.
>
> Also seen while narrowing it down: with `-m 32M` and `is_octal`, the application maps 31 MB ("Virtual address not enough for PSRAM, map as much as we can. 31MB is mapped") and then fails to map the flash (`E mmap: esp_mmu_map_virt(522): no such vaddr range`, `partition: load_partitions returned 0x105`); `-m 8M` works. That may be expected, but the documentation does not say which `-m` value to use with octal PSRAM.

## 5. espressif/qemu: el temporizador del sistema (`esp_timer`) no vuelve a 0 con un reinicio por software

- **Repositorio:** https://github.com/espressif/qemu
- **Versión:** QEMU 9.2.2 `esp_develop_9.2.2_20260417`, máquina `esp32s3`, ESP-IDF v6.1.
- **Ya reportado arriba:** no. [#200](https://github.com/espressif/qemu/issues/200) (abierto) es otro registro de SYSTEM que sobrevive al reinicio (la dirección de arranque del núcleo 1), no el SYSTIMER; [#120](https://github.com/espressif/qemu/issues/120) (cerrado) es el valor de `SYSTIMER_CONF_REG` del ESP32-C3 al encender.
- **Arreglo provisorio aquí:** `idf/components/gw_board/gw_io.c` (commit d1455c7, issue #74): la ventana del expansor simulado (`expander absent <ms>`) se mide desde el primer acceso de cada arranque y no con `esp_timer_get_time()` desde el arranque, porque tras un reinicio de fábrica la ventana ya estaba vencida. En el gateway, el shell mostraba `uptime 27 s` a los 2 s de un arranque posterior a un reinicio.

Texto propuesto (en inglés):

> **Title:** esp32s3: the system timer is not reset by esp_restart() (esp_timer_get_time() keeps counting across a software reset)
>
> **Version:** QEMU 9.2.2 (esp_develop_9.2.2_20260417), `-M esp32s3`, ESP-IDF v6.1
>
> On silicon, `esp_restart_noos()` resets the system timer together with the other peripherals (`SYSTEM_SYSTIMER_RST` in `components/esp_system/port/soc/esp32s3/system_internal.c`), so `esp_timer_get_time()` starts again from about 0 after `esp_restart()`, and so does the uptime that applications derive from it. In QEMU the counter keeps the value it had before the reset.
>
> **Steps:** the reproduction project above (boot 1 prints `esp_timer_get_time()` at `app_main` and calls `esp_restart()` 3 s later; boot 2 prints it again).
>
> **Expected:** about the same value at `app_main` in both boots (some 300 to 400 ms).
>
> **Observed** (5 runs out of 5): the second boot starts where the first one stopped; the panic reset that follows does not reset it either:
>
> ```
> rst:0x1 (POWERON),boot:0x4 (SPI_FLASH_BOOT)
> I (25) boot: ESP-IDF v6.1 2nd stage bootloader
> ...
> REPRO reset reason 1, esp_timer_get_time() at app_main: 386567 us
> REPRO esp_restart() at 3384520 us
> ...
> rst:0xc (RTC_SW_CPU_RST),boot:0x4 (SPI_FLASH_BOOT)
> I (1874) boot: ESP-IDF v6.1 2nd stage bootloader
> ...
> REPRO reset reason 3, esp_timer_get_time() at app_main: 3700571 us
> ...
> rst:0xc (RTC_SW_CPU_RST),boot:0x4 (SPI_FLASH_BOOT)
> I (4977) boot: ESP-IDF v6.1 2nd stage bootloader
> ...
> REPRO reset reason 4, esp_timer_get_time() at app_main: 6010117 us
> ```
>
> The bootloader's own log timestamps (`I (1874) boot:` instead of about `I (25)`) suggest that the CPU cycle counter it uses is not reset by the CPU reset either. Code that measures time windows from boot with `esp_timer` (timeouts, "N seconds after boot" logic) behaves differently in QEMU after a software reset than on the chip.

## 6. espressif/qemu: el *cache freeze* del ESP32-S3 no está modelado (aborta con "Bad ram pointer")

- **Repositorio:** https://github.com/espressif/qemu.
- **Versión:** `esp_develop_9.2.2_20260417` (la que instala `idf_tools.py install qemu-xtensa` con IDF v6.1), máquina `esp32s3`.
- **Ya reportado arriba:** no. [espressif/qemu#189](https://github.com/espressif/qemu/issues/189) (§3) es otra causa: `esp32s3_cpu_stall()` vacía, que usan el *panic* y el reinicio. El *cache freeze* no detiene el otro núcleo con `esp_cpu_stall()`: `esp_cache_freeze_caches_disable_interrupts()` (`esp_mm/esp_cache_utils.c`) solo congela la caché y cuenta con que el hardware haga esperar los accesos del otro núcleo. Las escrituras de flash sí lo detienen, pero por software (una tarea IPC que gira en IRAM), tampoco con `esp_cpu_stall()`. El PR [#190](https://github.com/espressif/qemu/pull/190) no debería cambiar esto: conviene un issue nuevo.
- **Arreglo provisorio aquí:** `CONFIG_GW_FW_PARK_OTHER_CORE` (solo en `sdkconfig.qemu`): mientras se verifica una imagen OTA, una tarea de prioridad alta deja al otro núcleo en `waiti` dentro de IRAM (`idf/components/gw_fw/gw_fw.c`, FW-1).

Texto propuesto (en inglés):

> **Title:** esp32s3: code fetched by the other core during a cache freeze aborts QEMU ("Bad ram pointer")
>
> **Version:** esp_develop_9.2.2_20260417, `-M esp32s3`, ESP-IDF v6.1
>
> On the ESP32-S3, `esp_mmu_map()`/`esp_mmu_unmap()` freeze the caches (`esp_cache_freeze_caches_disable_interrupts()`, `SOC_CACHE_FREEZE_SUPPORTED`) instead of stalling the other core: on the chip, a cache miss of the other core waits until the unfreeze. In QEMU, when the other core executes code from flash during that window, QEMU aborts with `qemu-system-xtensa: Bad ram pointer 0x42090f70` (the address is the code being executed, here `snprintf`). If the other core spins in IRAM instead, the freeze never completes (the caller hangs); only an other core halted in `waiti` lets it finish.
>
> **Steps:** an app with a task on core 1 that loops calling `snprintf()` (code in flash); on core 0, write an app image into the passive OTA slot, then call `esp_ota_end()` and `esp_ota_set_boot_partition()` (each maps every segment with `esp_image_verify()`). QEMU aborts within a few rounds (1 to 3 here); without the core 1 task it never does.
>
> **Expected:** accesses of the other core during a freeze wait until the unfreeze, as on the chip.
>
> A related abort, less frequent, happens during plain flash erases and writes (an OTA download writing about 1 MB): `../system/physmem.c:2517: iotlb_to_section: Assertion 'section_index < d->map.sections_nb' failed.` There IDF stalls the other core in IRAM (`spi_flash_disable_interrupts_caches_and_other_cpu`), so it may be a race of the memory map update between the vCPUs.
>
> With the other core halted in `waiti`, its tick interrupt still runs: the USB-Serial-JTAG connection monitor (`usb_serial_jtag_sof_tick_hook`, IRAM, a tick hook) then faults reading the peripheral at 0x60038014 during the freeze (`LoadStorePIFAddrError`), which never happens outside a freeze.
>
> The FreeRTOS tick itself does the same: `SysTickIsrHandler` (`freertos/port_systick.c:137`, `systimer_ll_clear_alarm_int()`) on the idle core faults with `LoadStorePIFAddrError` at 0x6002306c (SYSTIMER) while the other core has flash mapped, both during `esp_ota_end()` and when an app reads the other slot's `esp_app_desc_t` through `esp_ota_get_partition_description()`.

Seguimiento en este repositorio (issue #92): el harness del E2E reconoce el `LoadStorePIFAddrError` en un periférico mientras la app escribe o verifica una imagen y repite el escenario una vez; el mismo fallo en el *tick* fuera de una verificación (visto solo con *heap poisoning*, al leer la otra ranura) cuenta como del firmware y no se repite (`idf/README.md` §4.3). Un silencio se vuelca (`info registers -a` por el monitor de QEMU, dos veces, en `fault-*.txt`) pero nunca se repite. El silencio después de `Take trusted digest key(s)` que antes se sumaba a este reporte **no es del emulador**: era el desborde de la pila de la tarea `esp_timer` durante la verificación RSA-PSS del *rollback* por plazo (corregido en `fix/ota-trial`, e09b7b4). Su volcado (núcleo 0 quieto en una toma de spinlock con un puntero `0xffffffff`) encaja con eso. No va en el texto para espressif/qemu.

## 7. espressif/qemu: con MTTCG, escribir la MMU del ESP32-S3 desde un núcleo hace caer QEMU mientras el otro ejecuta (`Bad ram pointer`, SIGSEGV en `code_gen_buffer`)

- **Repositorio:** https://github.com/espressif/qemu.
- **Versión:** `QEMU emulator version 9.2.2 (esp_develop_9.2.2_20260417)`, máquina `esp32s3`, TCG multihilo (el valor por omisión con dos vCPU).
- **Visto en este repositorio (issue #107):** los SIGSEGV sin mensaje y los `Bad ram pointer 0x403764a6` / `0x4200xxxx` de los escenarios OTA, del #97 y de la prueba del #99/#101, siempre con carga en el host (otro E2E en paralelo). Los volcados de núcleo de QEMU (el harness los guarda desde el #107, `qemu/NNN-<log>/core` con `core-bt.txt`) dicen esto. En los tres SIGSEGV del E2E, un vCPU estaba dentro de `esp32s3_write_mmu_value()` (`hw/misc/esp32s3_cache.c:109`), releyendo la página de 64 KB con `blk_pread()` → `aio_poll()` en el *handler* MMIO de la escritura del registro de la MMU (`mmu_hal_map_region()` del firmware), y el otro se cayó dentro del código traducido (`code_gen_buffer`); en el firmware, ese núcleo estaba en `spi_flash_op_block_func()` (IRAM). En los tres *aborts* de la reproducción de abajo, el vCPU que aborta busca código por una dirección de la flash o de IRAM (`get_page_addr_code_hostp()` → `qemu_ram_addr_from_host_nofail()` → `Bad ram pointer`) mientras el otro sigue ejecutando, ya fuera del *handler*: la traducción de esa página quedó apuntando a memoria del host que ya no es RAM del huésped.
- **Relación con §6:** es el mismo hueco (el congelamiento de caché del S3 no está modelado), visto desde dentro de QEMU: el otro vCPU sigue corriendo durante la lectura bloqueante del *handler*. La reproducción de abajo no necesita ni OTA ni tareas en el otro núcleo.
- **Rodeo en este repositorio:** el harness arranca QEMU con `-accel tcg,thread=single` (los dos vCPU en un solo hilo del host; `E2E_TCG_THREAD=multi` vuelve a MTTCG).

Medido el 2026-11-04 en el contenedor de 4 CPU, con la aplicación de abajo, 9 corridas de 60 s, de a 3 en paralelo:

| Carga del host | MTTCG (por omisión) | `-accel tcg,thread=single` |
|---|---|---|
| Solo las 3 corridas | 0 caídas de 9 | 0 de 9 |
| Con 4 procesos que ocupan una CPU cada uno | **3 de 9** (`Bad ram pointer 0x4200dca3`, `0x4200dca6`, `0x4037acfa`, con *core*) | **0 de 9** (y la misma cantidad de *maps* por segundo) |

Texto propuesto (en inglés):

> **Title:** esp32s3: QEMU aborts ("Bad ram pointer") or segfaults in translated code while the other vCPU writes the MMU (MTTCG)
>
> **Version:** esp_develop_9.2.2_20260417, `-M esp32s3`, ESP-IDF v6.1, default TCG (multi-threaded)
>
> An app that maps and unmaps flash pages in a loop on core 0 (`esp_partition_mmap()` / `esp_partition_munmap()`), with core 1 only running its idle task, makes QEMU abort with `qemu-system-xtensa: Bad ram pointer 0x4200dca3` (`get_page_addr_code_hostp` → `qemu_ram_addr_from_host_nofail`, the aborting vCPU fetching code) within a minute when the host is loaded. In a larger app the same load gives SIGSEGVs inside `code_gen_buffer`; those core dumps show the other vCPU thread inside `esp32s3_write_mmu_value()` (`hw/misc/esp32s3_cache.c:109`), blocked in `blk_pread()` → `aio_poll()` while handling the MMU register write, so the MMU update (and its reload of the page from the flash image) races with the other vCPU's execution. With `-accel tcg,thread=single` it never happened in our runs.
>
> **Steps:** project with `partitions.csv`:
> ```
> nvs,      data, nvs,     0x9000,   0x6000
> phy_init, data, phy,     0xf000,   0x1000
> factory,  app,  factory, 0x10000,  0x100000
> blob,     data, 0x40,    0x110000, 0x200000
> ```
> `sdkconfig.defaults`: `CONFIG_IDF_TARGET="esp32s3"`, `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y`, `CONFIG_ESPTOOLPY_FLASHMODE_DIO=y`, `CONFIG_PARTITION_TABLE_CUSTOM=y`, `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"`, `CONFIG_ESP_TASK_WDT_EN=n`. `main/main.c`:
> ```c
> #include <stdio.h>
> #include <inttypes.h>
> #include "freertos/FreeRTOS.h"
> #include "freertos/task.h"
> #include "esp_partition.h"
> #include "esp_timer.h"
>
> static void mapper(void *arg)
> {
>     const esp_partition_t *part = arg;
>     for (uint32_t i = 0;; i++) {
>         const void *p;
>         esp_partition_mmap_handle_t h;
>         if (esp_partition_mmap(part, (i % 32) * 0x10000, 0x10000, ESP_PARTITION_MMAP_DATA, &p,
>                                &h) == ESP_OK) {
>             volatile uint8_t x = ((const uint8_t *)p)[i % 0x10000];
>             (void)x;
>             esp_partition_munmap(h);
>         }
>         if (i % 2000 == 0) {
>             printf("REPRO core %d: %" PRIu32 " maps at %" PRId64 " us\n", xPortGetCoreID(), i,
>                    esp_timer_get_time());
>         }
>     }
> }
>
> void app_main(void)
> {
>     const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "blob");
>     xTaskCreatePinnedToCore(mapper, "mapper", 4096, (void *)part, 5, NULL, 0);
> }
> ```
> Run `qemu-system-xtensa -M esp32s3 -m 32M -nographic -drive file=flash.bin,if=mtd,format=raw` (plus the usual eFuse drive) three at a time for 60 s each, with four busy loops on the host: 3 of 9 runs aborted here.
>
> **Expected:** no host crash; the other vCPU waits for (or is not affected by) the MMU update, as the cache freeze makes the other core wait on the chip.
