# SPDX-License-Identifier: Apache-2.0
#
# Host unit tests (QA-1) of the gateway's pure modules, run on native_sim.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(gw_unit_tests)

set(APP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)

list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)
# Generate from the firmware's proto so the include path ("proto/...") matches.
set(NANOPB_GENERATE_CPP_STANDALONE OFF)
nanopb_generate_cpp(proto_srcs proto_hdrs RELPATH ${APP_DIR} ${APP_DIR}/proto/sparkplug_b.proto)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${APP_DIR}/src)
target_sources(app PRIVATE ${proto_srcs} ${proto_hdrs})

target_sources(app PRIVATE
  ${APP_DIR}/src/config.c
  ${APP_DIR}/src/json_reader.c
  ${APP_DIR}/src/convert.c
  ${APP_DIR}/src/debounce.c
  ${APP_DIR}/src/model.c
  ${APP_DIR}/src/sparkplug.c
  ${APP_DIR}/src/spb_node.c
  ${APP_DIR}/src/status_led.c
  src/test_backoff_led.c
  src/test_config.c
  src/test_convert.c
  src/test_debounce.c
  src/test_rbe.c
  src/test_sparkplug.c
  src/test_spb_node.c
  src/test_util.c
)
