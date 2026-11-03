# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# The Waveshare board definition lives in this application (boards/waveshare).
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rs485_spb_gateway)

# Sparkplug B payloads: nanopb from the official Eclipse Tahu proto.
list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)
zephyr_nanopb_sources(app proto/sparkplug_b.proto)

# Default configuration embedded in the firmware (CFG-2). Override with
# -DGW_CONFIG_FILE=<path> to ship another default.
set(GW_CONFIG_FILE ${CMAKE_CURRENT_SOURCE_DIR}/config/default_config.json
    CACHE FILEPATH "JSON configuration embedded as the default")
generate_inc_file_for_target(app ${GW_CONFIG_FILE}
  ${ZEPHYR_BINARY_DIR}/include/generated/default_config.json.inc)

target_sources(app PRIVATE
  src/main.c
  src/app_config.c
  src/bdseq.c
  src/config.c
  src/convert.c
  src/debounce.c
  src/gw_shell.c
  src/gw_time.c
  src/json_reader.c
  src/local_io.c
  src/mb_poller.c
  src/model.c
  src/mqtt_link.c
  src/netmgr.c
  src/sparkplug.c
  src/spb_node.c
  src/status_led.c
)
