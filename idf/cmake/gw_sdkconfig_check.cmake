# Issue #83: an sdkconfig generated before an option was added to sdkconfig.defaults keeps the old
# value ("# CONFIG_X is not set" is a value, and kconfgen only applies defaults to symbols the
# file does not mention yet). The build checks a short list of options the firmware depends on
# against the defaults chain of this build (sdkconfig.defaults plus the overlays passed in
# SDKCONFIG_DEFAULTS, and their .<target> variants, the last file wins), so an overlay that sets
# one of them differently is respected and only a stale or hand-edited sdkconfig is reported.
#
# Included by the project CMakeLists.txt after project(); also runs in script mode for the host
# tests:
#   cmake -DSDKCONFIG=<file> "-DDEFAULTS=<file;file>" [-DIDF_TARGET=esp32s3] [-DMODE=warn] -P <this>
#
# An option that is absent from the sdkconfig is skipped: its dependencies are off, which is the
# configuration's decision (SPIRAM_IGNORE_NOTFOUND in the QEMU build, without PSRAM), not drift.
# MODE warn (-D GW_SDKCONFIG_CHECK=warn on the idf.py command line) reports without failing, for
# a deliberate local override.

set(GW_SDKCONFIG_REQUIRED
    # #80: without it, the QEMU build without PSRAM had no 32 KB block to validate a
    # configuration (PUT: 503); with PSRAM it keeps the internal RAM of idle TLS sessions free
    MBEDTLS_DYNAMIC_BUFFER
    # MQTT-4: mbedTLS allocations go to PSRAM (gw_storage's allocator, #135)
    MBEDTLS_CUSTOM_MEM_ALLOC
    # MB-2: RS485 DE released in the TX_DONE interrupt even while the cache is off
    UART_ISR_IN_IRAM
    # REL-1: task watchdog on the critical tasks and the idle tasks, with a panic
    ESP_TASK_WDT_EN
    ESP_TASK_WDT_INIT
    ESP_TASK_WDT_TIMEOUT_S
    ESP_TASK_WDT_PANIC
    ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
    ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
    # FW-1: a new image that never confirms itself goes back to the previous one
    BOOTLOADER_APP_ROLLBACK_ENABLE
    # Core dump to the "coredump" partition
    ESP_COREDUMP_ENABLE_TO_FLASH
    # #135: the firmware assumes the board's PSRAM (only the QEMU build turns it off)
    SPIRAM
    # #135: a board whose PSRAM is not detected still boots, only to report the fault (the
    # fault mode: network, web server and shell; the gateway does not start)
    SPIRAM_IGNORE_NOTFOUND
    # #122: no name buffer allocated on each microSD file operation (REL-2)
    FATFS_LFN_NONE
    # #201: the SNTP client gets all the servers of ntp.servers (gw_net.c checks it too)
    LWIP_SNTP_MAX_SERVERS)

# Reads CONFIG_ assignments of <file> into <prefix>_<NAME> variables (value "n" for "is not set",
# and for "=n" in a defaults file) and appends the names to <prefix>_NAMES.
function(__gw_sdkconfig_read file prefix)
    file(STRINGS "${file}" lines REGEX "^(CONFIG_[A-Za-z0-9_]+=|# CONFIG_[A-Za-z0-9_]+ is not set)")
    set(names ${${prefix}_NAMES})
    foreach(line IN LISTS lines)
        if(line MATCHES "^CONFIG_([A-Za-z0-9_]+)=(.*)$")
            set(name "${CMAKE_MATCH_1}")
            set(value "${CMAKE_MATCH_2}")
        elseif(line MATCHES "^# CONFIG_([A-Za-z0-9_]+) is not set")
            set(name "${CMAKE_MATCH_1}")
            set(value "n")
        else()
            continue()
        endif()
        string(STRIP "${value}" value)
        set(${prefix}_${name} "${value}" PARENT_SCOPE)
        list(APPEND names ${name})
    endforeach()
    set(${prefix}_NAMES ${names} PARENT_SCOPE)
endfunction()

# Sets <result> to the list of "NAME: sdkconfig has X, the defaults Y" mismatches.
function(gw_sdkconfig_mismatches sdkconfig defaults target result)
    set(expected_NAMES "")
    set(required ${GW_SDKCONFIG_REQUIRED})
    foreach(f IN LISTS defaults)
        foreach(g "${f}" "${f}.${target}")
            if(EXISTS "${g}")
                __gw_sdkconfig_read("${g}" expected)
            endif()
        endforeach()
    endforeach()
    set(active_NAMES "")
    __gw_sdkconfig_read("${sdkconfig}" active)
    set(out "")
    foreach(name IN LISTS required)
        if(NOT DEFINED expected_${name} OR NOT DEFINED active_${name})
            continue()
        endif()
        if(NOT "${active_${name}}" STREQUAL "${expected_${name}}")
            list(APPEND out "CONFIG_${name}: sdkconfig has ${active_${name}}, the defaults ${expected_${name}}")
        endif()
    endforeach()
    set(${result} "${out}" PARENT_SCOPE)
endfunction()

# Issue #103: combinations of options that the sdkconfig in use must not have, whatever the
# defaults say. NVS encryption (CFG-7) without flash encryption (FW-3), outside the emulator
# build: the reset-safe copy of an NVS migration (#94, secrets included) is a LittleFS file, and
# LittleFS is copy-on-write: once deleted (or overwritten) its blocks keep the plaintext in the
# "storage" partition until they are reused. Under flash encryption they are ciphertext.
function(gw_sdkconfig_forbidden sdkconfig result)
    set(active_NAMES "")
    __gw_sdkconfig_read("${sdkconfig}" active)
    set(out "")
    if("${active_NVS_ENCRYPTION}" STREQUAL "y" AND NOT "${active_SECURE_FLASH_ENC_ENABLED}" STREQUAL "y"
       AND NOT "${active_GW_IO_SIMULATED}" STREQUAL "y")
        list(APPEND out "CONFIG_NVS_ENCRYPTION=y without CONFIG_SECURE_FLASH_ENC_ENABLED: an NVS "
                        "migration leaves its copy of the secrets in clear in the free blocks of "
                        "LittleFS (issue #103); use the default build (flash encryption is not in "
                        "this project: issue #133, branch archive/efuse-production)")
    endif()
    set(${result} "${out}" PARENT_SCOPE)
endfunction()

function(gw_sdkconfig_check sdkconfig defaults target mode)
    gw_sdkconfig_forbidden("${sdkconfig}" forbidden)
    if(forbidden)
        list(JOIN forbidden "\n  " text)
        set(msg "${sdkconfig} has a combination the firmware refuses:\n  ${text}\n"
                "To build it anyway (a development board, knowingly), pass "
                "-D GW_SDKCONFIG_CHECK=warn.")
        string(CONCAT msg ${msg})
        if(mode STREQUAL "warn")
            message(WARNING "${msg}")
        else()
            message(FATAL_ERROR "${msg}")
        endif()
    endif()
    gw_sdkconfig_mismatches("${sdkconfig}" "${defaults}" "${target}" bad)
    if(NOT bad)
        return()
    endif()
    list(JOIN bad "\n  " text)
    set(msg "${sdkconfig} contradicts sdkconfig.defaults (or its overlays) in options the firmware "
            "requires (issue #83):\n  ${text}\n"
            "An sdkconfig keeps the values it was generated with, so options added to the defaults "
            "later do not reach it. Regenerate it: delete it and run 'idf.py reconfigure' (with the "
            "same -B/-D SDKCONFIG/SDKCONFIG_DEFAULTS as the build). To keep a deliberate local "
            "change, pass -D GW_SDKCONFIG_CHECK=warn.")
    string(CONCAT msg ${msg})
    if(mode STREQUAL "warn")
        message(WARNING "${msg}")
    else()
        message(FATAL_ERROR "${msg}")
    endif()
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    if(NOT IDF_TARGET)
        set(IDF_TARGET esp32s3)
    endif()
    gw_sdkconfig_check("${SDKCONFIG}" "${DEFAULTS}" "${IDF_TARGET}" "${MODE}")
endif()
