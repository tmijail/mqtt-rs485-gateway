# Host unit tests for the platform-independent gateway core (QA-1).
#
#   cmake -S idf/test/host -B build-host && cmake --build build-host -j3 && ctest --test-dir build-host
#
# Unity and cJSON are fetched at the versions used on target, pinned by commit hash (a tag
# can be moved; the hash cannot): Unity v2.6.1, cJSON v1.7.19.
cmake_minimum_required(VERSION 3.16)
project(gw_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(unity
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG cbcd08fa7de711053a3deec6339ee89cad5d2697)   # v2.6.1
FetchContent_Declare(cjson
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG c859b25da02955fef659d658b8f324b5cde87be3)   # v1.7.19
FetchContent_GetProperties(unity)
if(NOT unity_POPULATED)
    FetchContent_Populate(unity)
endif()
FetchContent_GetProperties(cjson)
if(NOT cjson_POPULATED)
    FetchContent_Populate(cjson)
endif()

add_library(unity STATIC ${unity_SOURCE_DIR}/src/unity.c)
target_include_directories(unity PUBLIC ${unity_SOURCE_DIR}/src)
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE UNITY_SUPPORT_64)

add_library(cjson STATIC ${cjson_SOURCE_DIR}/cJSON.c)
target_include_directories(cjson PUBLIC ${cjson_SOURCE_DIR})

set(IDF_PROJECT_DIR ${CMAKE_CURRENT_LIST_DIR}/../..)
add_subdirectory(${IDF_PROJECT_DIR}/components/nanopb nanopb)
add_subdirectory(${IDF_PROJECT_DIR}/components/gw_core gw_core)

enable_testing()
foreach(t test_mb_rtu test_mb_master test_conv test_logic test_config test_plan test_spb test_node
          test_poll test_dout test_netsel test_syslog test_auth test_cfgdoc test_button
          test_brokersel test_phost test_sf test_authlimit test_mbap test_localio test_buzz test_rtc
          test_ota test_nvsraw test_cardfmt test_sfcfg test_mbdiag test_cdump test_claim
          test_cfgschema test_bootloop test_fwlast test_lkgnet test_cfgprev test_wpa test_aplife
          test_ident test_logring)
    add_executable(${t} ${t}.c support.c)
    target_link_libraries(${t} PRIVATE gw_core unity)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
    target_compile_options(${t} PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(${t} PRIVATE
        GW_DEFAULT_CONFIG="${IDF_PROJECT_DIR}/components/gw_storage/default_config.json"
        GW_SPEC_EXAMPLE="${CMAKE_CURRENT_LIST_DIR}/spec_example.json")
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# Issue #145: the configuration hash of the spill does not depend on the C layout. The same
# suite again with every structure of the code under test packed and 1-byte enums (other sizes
# and offsets): the same golden value. cJSON and the sources that share structures with
# gw_config.c are compiled into it with the same flags; the rest comes from gw_core.
add_executable(test_sfcfg_packed test_sfcfg.c ${cjson_SOURCE_DIR}/cJSON.c
               ${IDF_PROJECT_DIR}/components/gw_core/src/gw_sfcfg.c
               ${IDF_PROJECT_DIR}/components/gw_core/src/gw_config.c
               ${IDF_PROJECT_DIR}/components/gw_core/src/gw_cfglint.c
               ${IDF_PROJECT_DIR}/components/gw_core/src/gw_conv.c
               ${IDF_PROJECT_DIR}/components/gw_core/src/gw_sfspill.c
               ${IDF_PROJECT_DIR}/components/gw_core/port/gw_port_posix.c)
target_link_libraries(test_sfcfg_packed PRIVATE gw_core unity)
target_include_directories(test_sfcfg_packed PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${cjson_SOURCE_DIR}
                           ${IDF_PROJECT_DIR}/components/gw_core/proto)
target_compile_options(test_sfcfg_packed PRIVATE -Wall -Wextra -Werror -fpack-struct -fshort-enums
                       -Wno-address-of-packed-member)
target_compile_definitions(test_sfcfg_packed PRIVATE GW_SFCFG_PACKED)
add_test(NAME test_sfcfg_packed COMMAND test_sfcfg_packed)

# Issue #145: the guard next to the canonical list fails the build when a field is added to
# gw_tag_cfg_t (a positional initializer one short, as after a new field, with the flags of
# gw_core). The firmware compiles gw_core with -Wall -Wextra -Werror too.
add_test(NAME sfcfg_guard
         COMMAND ${CMAKE_C_COMPILER} -std=c11 -fsyntax-only -Wall -Wextra -Werror
                 -DGW_SFCFG_GUARD_SELFTEST -I${IDF_PROJECT_DIR}/components/gw_core/include
                 -I${cjson_SOURCE_DIR} -I${IDF_PROJECT_DIR}/components/nanopb
                 ${IDF_PROJECT_DIR}/components/gw_core/src/gw_sfcfg.c)
set_tests_properties(sfcfg_guard PROPERTIES
                     PASS_REGULAR_EXPRESSION "error: missing initializer for field .write_max.")

# Writes representative Sparkplug payloads for the interoperability check
# (tools/spb_decode.py, decoded with the official proto and Python protobuf).
add_executable(spb_dump spb_dump.c support.c)
target_link_libraries(spb_dump PRIVATE gw_core unity)
target_include_directories(spb_dump PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(spb_dump PRIVATE
    GW_SPEC_EXAMPLE="${CMAKE_CURRENT_LIST_DIR}/spec_example.json")

# Issue #58: esp-mqtt 1.1.0 leaks network.if_name in esp_mqtt_set_config(); gw_edge must call it
# only through its guarded wrapper set_config().
add_test(NAME check_set_config
         COMMAND ${CMAKE_COMMAND} -DSRC=${IDF_PROJECT_DIR}/components/gw_edge/gw_edge.c
                 -P ${CMAKE_CURRENT_LIST_DIR}/check_set_config.cmake)

# Issue #167: the option lists of the web UI's configuration form are the value sets of the
# parser (a missing option silently changed a tag's word order on a save).
add_test(NAME check_web_lists
         COMMAND ${CMAKE_COMMAND} -DCONFIG_C=${IDF_PROJECT_DIR}/components/gw_core/src/gw_config.c
                 -DINDEX_HTML=${IDF_PROJECT_DIR}/components/gw_web/www/index.html
                 -P ${CMAKE_CURRENT_LIST_DIR}/check_web_lists.cmake)

# Issue #83: the build's check of the sdkconfig against sdkconfig.defaults and its overlays.
# fresh: matches; stale: DYNAMIC_BUFFER "is not set" from before #80 (fails, and only warns with
# MODE=warn); wdt5: a different watchdog timeout fails, unless an overlay of the build sets it;
# absent options (SPIRAM_IGNORE_NOTFOUND with SPIRAM off, as in the QEMU build) are skipped.
# An overlay with "@" is a file of the project. Issue #103: NVS encryption without flash
# encryption fails (reported with MODE=warn), except in the emulator build (GW_IO_SIMULATED).
# Issue #135: an sdkconfig without the PSRAM fails, except with the QEMU overlay (CONFIG_SPIRAM=n).
set(GW_CFGCHK ${IDF_PROJECT_DIR}/cmake/gw_sdkconfig_check.cmake)
set(GW_CFGCHK_DIR ${CMAKE_CURRENT_LIST_DIR}/sdkconfig_check)
set(GW_CFGCHK_DEFAULTS ${IDF_PROJECT_DIR}/sdkconfig.defaults)
foreach(c "fresh;fresh;;;0" "stale;stale;;warn;0;MBEDTLS_DYNAMIC_BUFFER" "stale;stale;;;1"
          "wdt5;wdt5;;;1" "wdt5_overlay;wdt5;wdt5.defaults;;0"
          "nvsenc_noflash;nvsenc_noflash;;;1"
          "nvsenc_noflash;nvsenc_noflash;;warn;0;NVS_ENCRYPTION"
          "nvsenc_sim;nvsenc_sim;@sdkconfig.qemu;;0"
          "nopsram;nopsram;;;1"
          "nopsram;nopsram;;warn;0;SPIRAM"
          "nopsram_qemu;nopsram;@sdkconfig.qemu;;0")
    list(GET c 0 name)
    list(GET c 1 cfg)
    list(GET c 2 overlay)
    list(GET c 3 mode)
    list(GET c 4 fails)
    set(expect "")
    if(mode STREQUAL "warn")
        list(GET c 5 expect)
        string(APPEND name "_warn")
    endif()
    set(defaults ${GW_CFGCHK_DEFAULTS})
    # $<SEMICOLON>: one argument with a list in it ("\;" kept its backslash, so before issue #93
    # an overlay was never read and wdt5_overlay passed without checking anything)
    if(overlay MATCHES "^@(.*)")
        string(APPEND defaults "$<SEMICOLON>${IDF_PROJECT_DIR}/${CMAKE_MATCH_1}")
    elseif(overlay)
        string(APPEND defaults "$<SEMICOLON>${GW_CFGCHK_DIR}/${overlay}")
    endif()
    add_test(NAME sdkconfig_check_${name}
             COMMAND ${CMAKE_COMMAND} -DSDKCONFIG=${GW_CFGCHK_DIR}/${cfg}.sdkconfig
                     "-DDEFAULTS=${defaults}" -DMODE=${mode} -P ${GW_CFGCHK})
    if(fails)
        set_tests_properties(sdkconfig_check_${name} PROPERTIES WILL_FAIL TRUE)
    elseif(mode STREQUAL "warn")
        set_tests_properties(sdkconfig_check_${name} PROPERTIES
                             PASS_REGULAR_EXPRESSION "CMake Warning.*CONFIG_${expect}")
    endif()
endforeach()
