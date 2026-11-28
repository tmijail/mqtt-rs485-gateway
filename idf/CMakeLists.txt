cmake_minimum_required(VERSION 3.22)

# Firmware version (FW-4): semantic version + git hash, shown in NBIRTH "Node Info/Firmware".
# Issue #279: -D GW_RELEASE=ON is the build of a release (.github/workflows/release.yml). Its
# version is GW_VERSION alone, the text of the tag v<GW_VERSION> without the "v". The
# configuration stops when git gives no commit or when idf/ has a change that is not in a
# commit: a release image comes from the commit of its tag only.
set(GW_VERSION "0.1.0")
option(GW_RELEASE "Build of a release: the version is GW_VERSION, without the git hash" OFF)
execute_process(COMMAND git rev-parse --short=8 HEAD
                WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                OUTPUT_VARIABLE GW_GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET RESULT_VARIABLE GW_GIT_RC)
execute_process(COMMAND git status --porcelain -- .
                WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                OUTPUT_VARIABLE GW_GIT_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
                RESULT_VARIABLE GW_GIT_STATUS_RC)
if(GW_RELEASE)
    if(NOT GW_GIT_RC EQUAL 0 OR NOT GW_GIT_HASH OR NOT GW_GIT_STATUS_RC EQUAL 0)
        message(FATAL_ERROR "GW_RELEASE=ON: git gives no commit and no status for "
                            "${CMAKE_CURRENT_LIST_DIR}. Build a release from a git clone.")
    endif()
    if(GW_GIT_DIRTY)
        message(FATAL_ERROR "GW_RELEASE=ON: the work tree has changes that are not in a "
                            "commit:\n${GW_GIT_DIRTY}\nBuild a release from a clean commit.")
    endif()
    set(PROJECT_VER "${GW_VERSION}")
elseif(GW_GIT_RC EQUAL 0 AND GW_GIT_HASH)
    set(PROJECT_VER "${GW_VERSION}+g${GW_GIT_HASH}")
    if(GW_GIT_DIRTY)
        set(PROJECT_VER "${PROJECT_VER}.dirty")
    endif()
else()
    set(PROJECT_VER "${GW_VERSION}+unknown")
endif()

# Directory packed into the LittleFS "storage" partition (see main/CMakeLists.txt).
set(GW_FS_DIR "${CMAKE_CURRENT_LIST_DIR}/fs" CACHE PATH "LittleFS image source directory")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Issue #300: the bootloader is a separate CMake project (bootloader_components/gw_boot_de). It
# gets the -D options of idf.py only through EXTRA_CMAKE_ARGS: the analyzer build examines the
# own bootloader component too (cmake/gw_warnings.cmake).
if(GW_ANALYZER)
    idf_build_set_property(EXTRA_CMAKE_ARGS "-DGW_ANALYZER=ON" APPEND)
endif()
project(rs485_spb_gateway)

# Issue #256: the debug lines of espcoredump (ESP_COREDUMP_LOGD, esp_core_dump_types.h) compile
# against LOG_LOCAL_LEVEL (CONFIG_LOG_MAXIMUM_LEVEL: verbose) and go out with esp_rom_printf(),
# which takes no console lock and ignores the run-time level: each summary of a dump wrote
# dozens of "D (...) esp_core_dump_elf:" lines into the lines of other tasks (as in #123). INFO
# (the level of a default ESP-IDF build) keeps the I, W and E lines ("Core dump has been saved
# to flash", which the E2E reads); do not set CONFIG_ESP_COREDUMP_LOGS=n, which removes them
# too. Without CONFIG_ESP_COREDUMP_ENABLE the component has no sources (an INTERFACE library):
# nothing to compile.
idf_component_get_property(gw_cdump_lib espcoredump COMPONENT_LIB)
get_target_property(gw_cdump_type ${gw_cdump_lib} TYPE)
if(NOT gw_cdump_type STREQUAL "INTERFACE_LIBRARY")
    target_compile_definitions(${gw_cdump_lib} PRIVATE LOG_LOCAL_LEVEL=ESP_LOG_INFO)
endif()

# Issue #83: IDF does not re-run the configuration when sdkconfig.defaults (or an overlay)
# changes, so an existing build kept the sdkconfig it had; kconfgen applies a new default to a
# value that it marked "# default:" only when it runs. The defaults files are now configure
# dependencies, and the configuration fails when the sdkconfig in use still contradicts them in
# an option the firmware requires (a value set by hand, or a file written without the markers);
# -D GW_SDKCONFIG_CHECK=warn only reports it.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/gw_sdkconfig_check.cmake)
idf_build_get_property(gw_sdkconfig SDKCONFIG)
idf_build_get_property(gw_sdkconfig_defaults SDKCONFIG_DEFAULTS)
idf_build_get_property(gw_idf_target IDF_TARGET)
foreach(f IN LISTS gw_sdkconfig_defaults)
    foreach(g "${f}" "${f}.${gw_idf_target}")
        if(EXISTS "${g}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${g}")
        endif()
    endforeach()
endforeach()
gw_sdkconfig_check("${gw_sdkconfig}" "${gw_sdkconfig_defaults}" "${gw_idf_target}"
                   "${GW_SDKCONFIG_CHECK}")
