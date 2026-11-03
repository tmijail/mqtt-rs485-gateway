cmake_minimum_required(VERSION 3.22)

# Firmware version (FW-4): semantic version + git hash, shown in NBIRTH "Node Info/Firmware".
set(GW_VERSION "0.1.0")
execute_process(COMMAND git rev-parse --short=8 HEAD
                WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                OUTPUT_VARIABLE GW_GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET RESULT_VARIABLE GW_GIT_RC)
execute_process(COMMAND git status --porcelain -- .
                WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                OUTPUT_VARIABLE GW_GIT_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(GW_GIT_RC EQUAL 0 AND GW_GIT_HASH)
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
project(rs485_spb_gateway)
