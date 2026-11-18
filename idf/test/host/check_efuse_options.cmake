# Issue #228 (#133): no build of main burns an eFuse on the board. Fails when sdkconfig.defaults,
# one of its .<target> variants or an overlay that is not for QEMU sets an option of the list in
# CLAUDE.md (Security constraints) to "y". The QEMU overlays (sdkconfig.qemu*) may set them: QEMU
# emulates the eFuses in a file. A generated sdkconfig (and sdkconfig.old) is not examined: the
# build's own check (cmake/gw_sdkconfig_check.cmake) examines the sdkconfig in use, for other
# options.
#   cmake -DDIR=<idf dir> -P check_efuse_options.cmake
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/gw_sdkconfig_check.cmake)   # __gw_sdkconfig_read

set(GW_EFUSE_OPTIONS
    SECURE_BOOT                        # Secure Boot V2 (FW-2): the digest of the key in eFuse
    SECURE_FLASH_ENC_ENABLED           # flash encryption (FW-3): the key and the counter in eFuse
    SECURE_DISABLE_ROM_DL_MODE         # no download mode, for ever
    SECURE_ENABLE_SECURE_ROM_DL_MODE   # the secure download mode, for ever
    BOOTLOADER_APP_ANTI_ROLLBACK       # the security version of the app in eFuse
    BOOT_ROM_LOG_ALWAYS_OFF            # the log of the ROM: an eFuse
    BOOT_ROM_LOG_ON_GPIO_HIGH
    BOOT_ROM_LOG_ON_GPIO_LOW
    NVS_ENCRYPTION                     # CFG-7: the HMAC key in eFuse, or flash encryption
    NVS_SEC_KEY_PROTECT_USING_HMAC)    # the HMAC scheme of the NVS encryption

file(GLOB files "${DIR}/sdkconfig.*")
list(SORT files)
set(examined "")
set(bad "")
foreach(f IN LISTS files)
    get_filename_component(name "${f}" NAME)
    if(name MATCHES "^sdkconfig\\.(qemu.*|old)$")
        continue()
    endif()
    list(APPEND examined "${name}")
    set(opt_NAMES "")
    __gw_sdkconfig_read("${f}" opt)
    foreach(o IN LISTS GW_EFUSE_OPTIONS)
        if("${opt_${o}}" STREQUAL "y")
            list(APPEND bad "${name}: CONFIG_${o}=y")
        endif()
        unset(opt_${o})
    endforeach()
endforeach()
list(FIND examined "sdkconfig.defaults" at)
if(at EQUAL -1)
    message(FATAL_ERROR "${DIR}: no sdkconfig.defaults")
endif()
if(bad)
    list(JOIN bad "\n  " text)
    message(FATAL_ERROR "options that burn an eFuse, outside a QEMU overlay (#133, #228):\n  "
                        "${text}\nThey are only on the branch archive/efuse-production.")
endif()
list(JOIN examined ", " text)
message(STATUS "no option that burns an eFuse in ${text}")
