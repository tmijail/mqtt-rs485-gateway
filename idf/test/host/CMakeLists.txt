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
          test_poll test_dout)
    add_executable(${t} ${t}.c support.c)
    target_link_libraries(${t} PRIVATE gw_core unity)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
    target_compile_options(${t} PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(${t} PRIVATE
        GW_DEFAULT_CONFIG="${IDF_PROJECT_DIR}/components/gw_storage/default_config.json"
        GW_SPEC_EXAMPLE="${CMAKE_CURRENT_LIST_DIR}/spec_example.json")
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# Writes representative Sparkplug payloads for the interoperability check
# (tools/spb_decode.py, decoded with the official proto and Python protobuf).
add_executable(spb_dump spb_dump.c support.c)
target_link_libraries(spb_dump PRIVATE gw_core unity)
target_include_directories(spb_dump PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(spb_dump PRIVATE
    GW_SPEC_EXAMPLE="${CMAKE_CURRENT_LIST_DIR}/spec_example.json")
