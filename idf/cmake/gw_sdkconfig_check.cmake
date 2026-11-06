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
    # #80: without it, no PSRAM leaves no 32 KB block to validate a configuration (PUT: 503)
    MBEDTLS_DYNAMIC_BUFFER
    # MQTT-4: mbedTLS allocations go to PSRAM when present (gw_storage's allocator)
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
    # The board boots without PSRAM when it is not detected
    SPIRAM_IGNORE_NOTFOUND
    # #122: no name buffer allocated on each microSD file operation (REL-2)
    FATFS_LFN_NONE)

# Issue #93: checked only when sdkconfig.production is in the defaults chain. A production
# sdkconfig generated before one of these was set (or edited by hand) would build an image that
# burns eFuses without the protection the overlay promises (FW-2, FW-3, CFG-7). An option whose
# parent is off is absent and skipped (the QEMU Secure Boot run turns flash encryption off in its
# own overlay, so the release mode is absent); the parents are in the list.
set(GW_SDKCONFIG_REQUIRED_PRODUCTION
    SECURE_BOOT
    SECURE_BOOT_V2_ENABLED
    SECURE_FLASH_ENC_ENABLED
    SECURE_FLASH_ENCRYPTION_MODE_RELEASE
    SECURE_DISABLE_ROM_DL_MODE
    NVS_ENCRYPTION
    NVS_SEC_KEY_PROTECT_USING_HMAC)

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
        get_filename_component(base "${f}" NAME)
        if(base STREQUAL "sdkconfig.production")
            list(APPEND required ${GW_SDKCONFIG_REQUIRED_PRODUCTION})
        endif()
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

# Directory a relative CONFIG_PARTITION_TABLE_CUSTOM_FILENAME is resolved against: the project
# (the parent of this file's directory) unless GW_SDKCONFIG_PROJECT_DIR (script mode: PROJECT_DIR)
# says otherwise (host tests).
set(__gw_cfgchk_project "${CMAKE_CURRENT_LIST_DIR}/..")

# Issue #108 B7: sets <result> to the partitions of the CSV <file> named in <names> whose flags
# lack "encrypted" (or to the reason the file cannot be checked).
function(__gw_partitions_unencrypted file names result)
    if(NOT EXISTS "${file}")
        set(${result} "partition table ${file} not found" PARENT_SCOPE)
        return()
    endif()
    file(STRINGS "${file}" lines)
    set(out "")
    foreach(line IN LISTS lines)
        string(STRIP "${line}" line)
        if(line STREQUAL "" OR line MATCHES "^#")
            continue()
        endif()
        # Name, Type, SubType, Offset, Size, Flags (no list(): empty fields, policy CMP0007)
        if(NOT line MATCHES "^([^,]*),[^,]*,[^,]*,[^,]*,[^,]*(,(.*))?$")
            continue()
        endif()
        string(STRIP "${CMAKE_MATCH_1}" name)
        set(flags "${CMAKE_MATCH_3}")
        list(FIND names "${name}" i)
        if(i LESS 0)
            continue()
        endif()
        if(NOT flags MATCHES "encrypted")
            list(APPEND out "${name}")
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
                        "LittleFS (issue #103); use sdkconfig.production, or the default build")
    endif()
    # issue #108 B7: flash encryption covers only the partitions flagged "encrypted": storage
    # (LittleFS, the migration copy, certificates) and coredump (RAM with the secrets) must be
    if("${active_NVS_ENCRYPTION}" STREQUAL "y" AND "${active_SECURE_FLASH_ENC_ENABLED}" STREQUAL "y"
       AND "${active_PARTITION_TABLE_CUSTOM}" STREQUAL "y")
        string(REGEX REPLACE "^\"(.*)\"$" "\\1" csv "${active_PARTITION_TABLE_CUSTOM_FILENAME}")
        if(NOT IS_ABSOLUTE "${csv}")
            if(DEFINED GW_SDKCONFIG_PROJECT_DIR)
                set(csv "${GW_SDKCONFIG_PROJECT_DIR}/${csv}")
            else()
                set(csv "${__gw_cfgchk_project}/${csv}")
            endif()
        endif()
        __gw_partitions_unencrypted("${csv}" "storage;coredump" plain)
        if(plain)
            list(JOIN plain ", " plain)
            list(APPEND out "flash encryption with NVS encryption, but ${plain} not flagged "
                            "\"encrypted\" in ${csv}: they would stay in clear (issue #108), use "
                            "partitions-production.csv")
        endif()
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
    if(PROJECT_DIR)
        set(GW_SDKCONFIG_PROJECT_DIR "${PROJECT_DIR}")
    endif()
    gw_sdkconfig_check("${SDKCONFIG}" "${DEFAULTS}" "${IDF_TARGET}" "${MODE}")
endif()
