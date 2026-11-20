# Issue #226: libFuzzer targets for the parsers of gw_core (data from the RS485 bus, Modbus TCP,
# MQTT, the web UI and the REST API, the flash, the RTC memory, the microSD card and, #274, the
# DNS queries and the HTTP requests of the clients of the access point).
#
#   cmake -S idf/test/fuzz -B build-fuzz -DCMAKE_C_COMPILER=clang && cmake --build build-fuzz -j
#   idf/test/fuzz/run_fuzz.sh build-fuzz 60          # each target 60 s, $(nproc) at a time
#   build-fuzz/fuzz_spb -dict=idf/test/fuzz/spb.dict build-fuzz/corpus/fuzz_spb build-fuzz/seeds/fuzz_spb
#
# Only clang has libFuzzer. Every source (gw_core, cJSON, nanopb and the targets) is compiled with
# -fsanitize=fuzzer-no-link,address,undefined; the targets link with -fsanitize=fuzzer. cJSON is
# the same commit as in test/host (v1.7.19).
cmake_minimum_required(VERSION 3.16)
project(gw_fuzz C)

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "the fuzz targets need clang (libFuzzer): -DCMAKE_C_COMPILER=clang")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    add_compile_options(-O2 -g)   # fast, with line numbers in the reports; no NDEBUG
endif()

set(GW_FUZZ_SANITIZE -fsanitize=address,undefined -fno-sanitize-recover=all
                     -fno-omit-frame-pointer)
add_compile_options(-fsanitize=fuzzer-no-link ${GW_FUZZ_SANITIZE})

include(FetchContent)
FetchContent_Declare(cjson
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG c859b25da02955fef659d658b8f324b5cde87be3)   # v1.7.19, as test/host
FetchContent_GetProperties(cjson)
if(NOT cjson_POPULATED)
    FetchContent_Populate(cjson)
endif()
add_library(cjson STATIC ${cjson_SOURCE_DIR}/cJSON.c)
target_include_directories(cjson PUBLIC ${cjson_SOURCE_DIR})

set(IDF_PROJECT_DIR ${CMAKE_CURRENT_LIST_DIR}/../..)
include(${IDF_PROJECT_DIR}/cmake/gw_warnings.cmake)
add_subdirectory(${IDF_PROJECT_DIR}/components/nanopb nanopb)
add_subdirectory(${IDF_PROJECT_DIR}/components/gw_core gw_core)

# The configuration documents of the repository: the configurations of some targets, and seeds.
set(GW_FUZZ_DOCS ${IDF_PROJECT_DIR}/tools/e2e/fs/config.json
                 ${CMAKE_CURRENT_LIST_DIR}/../host/spec_example.json
                 ${IDF_PROJECT_DIR}/components/gw_storage/default_config.json)
set(GW_FUZZ_DEFS GW_FUZZ_E2E_CONFIG="${IDF_PROJECT_DIR}/tools/e2e/fs/config.json"
                 GW_FUZZ_SPEC_EXAMPLE="${CMAKE_CURRENT_LIST_DIR}/../host/spec_example.json"
                 GW_FUZZ_DEFAULT_CONFIG="${IDF_PROJECT_DIR}/components/gw_storage/default_config.json")

set(GW_FUZZ_TARGETS fuzz_mb_rtu fuzz_mbap fuzz_spb fuzz_node fuzz_config fuzz_cfgdoc
                    fuzz_web_json fuzz_web_text fuzz_flash fuzz_nvs fuzz_text fuzz_sfspill
                    fuzz_captive)
foreach(t ${GW_FUZZ_TARGETS})
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE gw_core)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_LIST_DIR}
                               ${IDF_PROJECT_DIR}/components/gw_core/proto)
    target_compile_options(${t} PRIVATE ${GW_HOST_WARNING_FLAGS})
    target_compile_definitions(${t} PRIVATE ${GW_FUZZ_DEFS})
    target_link_options(${t} PRIVATE -fsanitize=fuzzer ${GW_FUZZ_SANITIZE})
endforeach()

# The seed corpus of each target, in <build>/seeds/<target>: valid inputs that fuzz_seeds makes
# with the encoders of gw_core (so that they follow a change of a format) and from the
# configuration documents of the repository.
add_executable(fuzz_seeds fuzz_seeds.c)
target_link_libraries(fuzz_seeds PRIVATE gw_core)
target_compile_options(fuzz_seeds PRIVATE ${GW_HOST_WARNING_FLAGS})
target_include_directories(fuzz_seeds PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_options(fuzz_seeds PRIVATE ${GW_FUZZ_SANITIZE})
target_compile_definitions(fuzz_seeds PRIVATE ${GW_FUZZ_DEFS})

set(GW_FUZZ_SEEDS ${CMAKE_BINARY_DIR}/seeds)
add_custom_command(OUTPUT ${GW_FUZZ_SEEDS}/.stamp
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${GW_FUZZ_SEEDS}
    COMMAND fuzz_seeds ${GW_FUZZ_SEEDS}
    COMMAND ${CMAKE_COMMAND} -E touch ${GW_FUZZ_SEEDS}/.stamp
    DEPENDS fuzz_seeds ${GW_FUZZ_DOCS}
    COMMENT "Seed corpus of the fuzz targets")
add_custom_target(fuzz_seed_corpus ALL DEPENDS ${GW_FUZZ_SEEDS}/.stamp)
