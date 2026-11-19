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

# Issue #225: AddressSanitizer and UndefinedBehaviorSanitizer on every target (compile and link),
# also Unity, cJSON, nanopb and gw_core. -fno-sanitize-recover=all: the first finding stops the
# test with a non-zero exit, so ctest reports it as a failure.
#
#   cmake -S idf/test/host -B build-asan -DGW_SANITIZE=ON
option(GW_SANITIZE "Build the host tests with -fsanitize=address,undefined" OFF)
if(GW_SANITIZE)
    set(GW_SANITIZE_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=all
                          -fno-omit-frame-pointer)
    add_compile_options(${GW_SANITIZE_FLAGS})
    add_link_options(${GW_SANITIZE_FLAGS})
endif()

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
# issue #227: the warnings of the own components, plus -Wundef (GW_HOST_WARNING_FLAGS)
include(${IDF_PROJECT_DIR}/cmake/gw_warnings.cmake)
add_subdirectory(${IDF_PROJECT_DIR}/components/nanopb nanopb)
add_subdirectory(${IDF_PROJECT_DIR}/components/gw_core gw_core)

# Issue #228: the call graph of gw_core and nanopb for the rule checks (idf/tools/check_rules.py,
# at the end of this file). GCC writes <src>.c.ci next to each object; the generated code does
# not change. GCC 10 or later. -DGW_RULE_CHECKS=OFF (another compiler, no Python) removes the
# three checks of the call graph; the CI keeps them on.
option(GW_RULE_CHECKS "The rule checks of the call graph (issue #228; needs GCC and Python 3)" ON)
if(GW_RULE_CHECKS)
    find_package(Python3 COMPONENTS Interpreter)
    if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_VERSION VERSION_LESS 10
       OR NOT Python3_Interpreter_FOUND OR NOT CMAKE_NM)
        message(FATAL_ERROR "The rule checks (issue #228) need GCC 10 or later, Python 3 and nm. "
                            "Without them, configure with -DGW_RULE_CHECKS=OFF.")
    endif()
    target_compile_options(gw_core PRIVATE -fcallgraph-info=su)
    target_compile_options(nanopb PRIVATE -fcallgraph-info=su)
endif()

# The helpers of the tests (support.c), compiled one time. Issue #231: one entry for each source
# in compile_commands.json, for clang-tidy.
add_library(test_support STATIC support.c)
target_link_libraries(test_support PUBLIC gw_core unity)
target_include_directories(test_support PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(test_support PRIVATE ${GW_HOST_WARNING_FLAGS})
target_compile_definitions(test_support PRIVATE
    GW_SPEC_EXAMPLE="${CMAKE_CURRENT_LIST_DIR}/spec_example.json")

enable_testing()
foreach(t test_mb_rtu test_mb_master test_conv test_logic test_config test_plan test_spb test_node
          test_poll test_dout test_netsel test_syslog test_auth test_cfgdoc test_button
          test_brokersel test_phost test_sf test_authlimit test_mbap test_localio test_buzz test_rtc
          test_ota test_nvsraw test_cardfmt test_sfcfg test_mbdiag test_cdump test_claim
          test_cfgschema test_bootloop test_fwlast test_lkgnet test_cfgprev test_wpa test_aplife
          test_ident test_logring test_mblocal test_wifiown test_certs)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE test_support)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
    target_compile_options(${t} PRIVATE ${GW_HOST_WARNING_FLAGS})
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
# Issue #231: clang-tidy examines the sources of gw_core with the flags of gw_core, not with these.
# A second entry of a source in compile_commands.json also gave false results (clang-tidy 18
# examines each entry of a file in one process, and its valist check then fails).
set_target_properties(test_sfcfg_packed PROPERTIES EXPORT_COMPILE_COMMANDS OFF)
if(GW_SANITIZE)
    # Issue #225: -fpack-struct puts the members at unaligned offsets on purpose (#145), and
    # x86-64 permits unaligned access. UBSan's alignment check would stop at the first store
    # (gw_config.c, a double in a packed gw_tag_cfg_t). The other checks stay on.
    target_compile_options(test_sfcfg_packed PRIVATE -fno-sanitize=alignment)
endif()
add_test(NAME test_sfcfg_packed COMMAND test_sfcfg_packed)

# Issue #145: the guard next to the canonical list fails the build when a field is added to
# gw_tag_cfg_t (a positional initializer one short, as after a new field, with the flags of
# gw_core). The firmware compiles gw_core with -Wall -Wextra -Werror too. -std=gnu11 is the
# standard of the host build (CMAKE_C_STANDARD 11 with extensions): with -std=c11, strnlen() has
# no declaration. The quotes around the field name are ASCII or UTF-8, after the locale.
add_test(NAME sfcfg_guard
         COMMAND ${CMAKE_C_COMPILER} -std=gnu11 -fsyntax-only -Wall -Wextra -Werror
                 -DGW_SFCFG_GUARD_SELFTEST -I${IDF_PROJECT_DIR}/components/gw_core/include
                 -I${cjson_SOURCE_DIR} -I${IDF_PROJECT_DIR}/components/nanopb
                 ${IDF_PROJECT_DIR}/components/gw_core/src/gw_sfcfg.c)
set_tests_properties(sfcfg_guard PROPERTIES
                     PASS_REGULAR_EXPRESSION "error: missing initializer for field [^ ]*write_max")

# Writes representative Sparkplug payloads for the interoperability check
# (tools/spb_decode.py, decoded with the official proto and Python protobuf).
add_executable(spb_dump spb_dump.c)
target_link_libraries(spb_dump PRIVATE test_support)
target_compile_options(spb_dump PRIVATE ${GW_HOST_WARNING_FLAGS})
target_include_directories(spb_dump PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(spb_dump PRIVATE
    GW_SPEC_EXAMPLE="${CMAKE_CURRENT_LIST_DIR}/spec_example.json")

# Issue #58: esp-mqtt 1.1.0 leaks network.if_name in esp_mqtt_set_config(); gw_edge must call it
# only through its guarded wrapper set_config().
add_test(NAME check_set_config
         COMMAND ${CMAKE_COMMAND} -DSRC=${IDF_PROJECT_DIR}/components/gw_edge/gw_edge.c
                 -P ${CMAKE_CURRENT_LIST_DIR}/check_set_config.cmake)

# Issue #227: each CONFIG_GW_ name in the sources and in the sdkconfig files is an option of a
# Kconfig file (the firmware can not use -Wundef, docs/reference/compiler-warnings-and-stack.md); the self-test adds a misspelled one.
add_test(NAME check_kconfig_names
         COMMAND ${CMAKE_COMMAND} -DROOT=${IDF_PROJECT_DIR}
                 -P ${CMAKE_CURRENT_LIST_DIR}/check_kconfig_names.cmake)
add_test(NAME check_kconfig_names_selftest
         COMMAND ${CMAKE_COMMAND} -DROOT=${IDF_PROJECT_DIR}
                 -DEXTRA=${CMAKE_CURRENT_LIST_DIR}/kconfig_names/misspelled.h
                 -P ${CMAKE_CURRENT_LIST_DIR}/check_kconfig_names.cmake)
set_tests_properties(check_kconfig_names_selftest PROPERTIES
                     PASS_REGULAR_EXPRESSION "misspelled.h: CONFIG_GW_IO_SIMULATD")

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
# encryption fails (reported with MODE=warn), except in the emulator build (GW_IO_SIMULATED; issue
# #261: only its overlay sdkconfig.qemu-secure turns the NVS encryption on).
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

# Issue #228: the checks of the rules of CLAUDE.md. Each check has a self-test: a small bad
# example (rules/selftest/, rules/efuse/) that the check must refuse. A name with "good_" in the
# report of a self-test is a false positive, and fails it.
#   check_noalloc   REL-2: no allocation, no cJSON and no JSON lock in the polling, encoding or
#                   publish path (the files of rules/noalloc_path.txt and each function that
#                   they call; nm and the call graph)
#   check_jsonlock  #106: no cJSON outside gw_core; in gw_core, each path to a cJSON call passes
#                   gw_port_json_lock() (the call graph)
#   check_rs485     uart_* and mb_master_* only in gw_bus and gw_rs485; the functions of gw_core
#                   that reach the Modbus master only from them (rules/rs485_exceptions.txt)
#   check_efuse_options  #133: no option that burns an eFuse outside a QEMU overlay
# The details and the limits are in idf/tools/check_rules.py and check_efuse_options.cmake.
add_test(NAME check_efuse_options
         COMMAND ${CMAKE_COMMAND} -DDIR=${IDF_PROJECT_DIR}
                 -P ${CMAKE_CURRENT_LIST_DIR}/check_efuse_options.cmake)
add_test(NAME check_efuse_options_selftest
         COMMAND ${CMAKE_COMMAND} -DDIR=${CMAKE_CURRENT_LIST_DIR}/rules/efuse
                 -P ${CMAKE_CURRENT_LIST_DIR}/check_efuse_options.cmake)
set_tests_properties(check_efuse_options_selftest PROPERTIES
    PASS_REGULAR_EXPRESSION
        "sdkconfig.defaults.esp32s3: CONFIG_SECURE_BOOT=y.*sdkconfig.release: CONFIG_BOOT_ROM_LOG_ON_GPIO_HIGH=y.*sdkconfig.release: CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC=y"
    FAIL_REGULAR_EXPRESSION "qemu-good|SECURE_FLASH_ENC|no option that burns")

if(GW_RULE_CHECKS)
    set(GW_RULES ${Python3_EXECUTABLE} ${IDF_PROJECT_DIR}/tools/check_rules.py)
    set(GW_RULES_DIR ${CMAKE_CURRENT_LIST_DIR}/rules)
    add_library(rules_selftest STATIC rules/selftest/bad_path.c rules/selftest/bad_alloc.c
                                      rules/selftest/bad_json.c rules/selftest/bad_bus.c)
    target_link_libraries(rules_selftest PRIVATE cjson)
    target_include_directories(rules_selftest PRIVATE
                               ${IDF_PROJECT_DIR}/components/gw_core/include)
    target_compile_options(rules_selftest PRIVATE -fcallgraph-info=su)
    set(core_objs "$<TARGET_OBJECTS:gw_core>$<SEMICOLON>$<TARGET_OBJECTS:nanopb>")
    set(bad_objs "$<TARGET_OBJECTS:rules_selftest>")

    add_test(NAME check_noalloc
             COMMAND ${GW_RULES} noalloc "--objects=${core_objs}" --nm ${CMAKE_NM}
                     --list ${GW_RULES_DIR}/noalloc_path.txt)
    add_test(NAME check_noalloc_selftest
             COMMAND ${GW_RULES} noalloc "--objects=${bad_objs}" --nm ${CMAKE_NM}
                     --list ${GW_RULES_DIR}/selftest/noalloc_path.txt)
    set_tests_properties(check_noalloc_selftest PROPERTIES
        PASS_REGULAR_EXPRESSION
            "nm: bad_path.c refers to free.*bad_path.c:bad_poll -> bad_alloc.c:bad_alloc_helper -> calloc"
        FAIL_REGULAR_EXPRESSION "good_")

    add_test(NAME check_jsonlock
             COMMAND ${GW_RULES} jsonlock "--objects=$<TARGET_OBJECTS:gw_core>"
                     --root ${IDF_PROJECT_DIR} --core gw_core)
    add_test(NAME check_jsonlock_selftest
             COMMAND ${GW_RULES} jsonlock "--objects=${bad_objs}"
                     --root ${GW_RULES_DIR}/selftest --core gw_core)
    set_tests_properties(check_jsonlock_selftest PROPERTIES
        PASS_REGULAR_EXPRESSION
            "gw_web/bad_web.c:7: cJSON_Parse outside gw_core.*bad_json.c:bad_json_locked calls gw_port_json_lock and never gw_port_json_unlock.*bad_json.c:bad_json_parse -> bad_json.c:parse_unlocked calls cJSON_Parse"
        FAIL_REGULAR_EXPRESSION "good_")

    add_test(NAME check_rs485
             COMMAND ${GW_RULES} rs485 "--objects=$<TARGET_OBJECTS:gw_core>"
                     --root ${IDF_PROJECT_DIR} --core gw_core --owners gw_bus,gw_rs485
                     --exceptions ${GW_RULES_DIR}/rs485_exceptions.txt)
    add_test(NAME check_rs485_selftest
             COMMAND ${GW_RULES} rs485 "--objects=${bad_objs}"
                     --root ${GW_RULES_DIR}/selftest --core gw_core --owners gw_bus,gw_rs485
                     --exceptions ${GW_RULES_DIR}/selftest/rs485_exceptions.txt)
    set_tests_properties(check_rs485_selftest PROPERTIES
        PASS_REGULAR_EXPRESSION
            "bad_bus.c:bad_uart_core calls uart_flush.*gw_web/bad_web.c:8: bad_bus_entry outside.*gw_web/bad_web.c:9: gw_rs485_init outside.*gw_web/bad_web.c:10: uart_write_bytes outside.*the exception main/main.c gw_rs485_init is not used"
        FAIL_REGULAR_EXPRESSION "good_")
endif()
