# Issue #227: fails when a source or an sdkconfig file names a CONFIG_GW_ option that no Kconfig
# file of the project declares. ESP-IDF leaves a disabled bool option undefined, so a misspelled
# "#if CONFIG_GW_..." or "#ifdef CONFIG_GW_..." is 0 without a word. -Wundef can not find it in the
# firmware: the headers of ESP-IDF itself give thousands of -Wundef warnings
# (docs/reference/compiler-warnings-and-stack.md#no--wundef-in-the-firmware).
#   cmake -DROOT=<idf dir> [-DEXTRA=<one more file to examine>] -P check_kconfig_names.cmake
file(GLOB kconfigs "${ROOT}/components/*/Kconfig*" "${ROOT}/main/Kconfig*")
set(known "")
foreach(k ${kconfigs})
    file(STRINGS "${k}" lines REGEX "^[ \t]*(menu)?config[ \t]+GW_[A-Z0-9_]+")
    foreach(l ${lines})
        string(REGEX REPLACE "^[ \t]*(menu)?config[ \t]+(GW_[A-Z0-9_]+).*" "CONFIG_\\2" name "${l}")
        list(APPEND known "${name}")
    endforeach()
endforeach()
list(LENGTH known n_known)
if(n_known EQUAL 0)
    message(FATAL_ERROR "no GW_ option in the Kconfig files of ${ROOT}")
endif()
file(GLOB_RECURSE srcs "${ROOT}/components/gw_*/*.c" "${ROOT}/components/gw_*/*.h"
                       "${ROOT}/main/*.c" "${ROOT}/main/*.h")
file(GLOB cfgs "${ROOT}/sdkconfig.defaults*" "${ROOT}/sdkconfig.qemu*")
set(bad "")
foreach(f ${srcs} ${cfgs} ${EXTRA})
    file(READ "${f}" text)
    # "CONFIG_GW_X_" in a comment that names a group of options is not a name
    string(REGEX MATCHALL "CONFIG_GW_[A-Z0-9_]*[A-Z0-9]" used "${text}")
    list(REMOVE_DUPLICATES used)
    foreach(u ${used})
        list(FIND known "${u}" at)
        if(at EQUAL -1)
            list(APPEND bad "${f}: ${u}")
        endif()
    endforeach()
endforeach()
if(bad)
    list(JOIN bad "\n  " text)
    message(FATAL_ERROR "options that no Kconfig file declares (issue #227):\n  ${text}")
endif()
message(STATUS "each CONFIG_GW_ name is declared (${n_known} options)")
