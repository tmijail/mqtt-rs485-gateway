# nanopb 0.4.9.1 runtime, vendored verbatim from https://github.com/nanopb/nanopb
# (tag nanopb-0.4.9.1, Zlib license, see LICENSE.txt). Only the three runtime
# sources and their headers are kept. No compile definitions are set, so the
# struct layout seen by every consumer is the library default.
#
# Usable both as an ESP-IDF component and as a plain CMake library (host tests).
if(ESP_PLATFORM)
    idf_component_register(SRCS "pb_common.c" "pb_encode.c" "pb_decode.c"
                           INCLUDE_DIRS ".")
else()
    add_library(nanopb STATIC pb_common.c pb_encode.c pb_decode.c)
    target_include_directories(nanopb PUBLIC ${CMAKE_CURRENT_LIST_DIR})
endif()
