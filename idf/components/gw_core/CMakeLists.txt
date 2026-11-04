# gw_core: platform-independent gateway logic (config, Modbus RTU, conversions,
# report by exception, Sparkplug codec and edge-node engine).
# Builds as an ESP-IDF component or, outside IDF, as a plain static library for
# the host unit tests (test/host).
set(GW_CORE_SRCS
    src/gw_auth.c
    src/gw_authlimit.c
    src/gw_backoff.c
    src/gw_brokersel.c
    src/gw_button.c
    src/gw_buzz.c
    src/gw_cfgdoc.c
    src/gw_config.c
    src/gw_conv.c
    src/gw_debounce.c
    src/gw_mbap.c
    src/gw_devstate.c
    src/gw_dout.c
    src/gw_localio.c
    src/gw_netsel.c
    src/gw_node.c
    src/gw_nvsraw.c
    src/gw_ota.c
    src/gw_phost.c
    src/gw_plan.c
    src/gw_poll.c
    src/gw_rbe.c
    src/gw_sf.c
    src/gw_sfspill.c
    src/gw_syslog.c
    src/gw_tagdb.c
    src/mb_master.c
    src/mb_rtu.c
    src/spb.c
    proto/sparkplug_b.pb.c)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${GW_CORE_SRCS} port/gw_port_esp.c
                           INCLUDE_DIRS include
                           PRIV_INCLUDE_DIRS proto
                           REQUIRES log
                           PRIV_REQUIRES nanopb freertos)
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
else()
    add_library(gw_core STATIC ${GW_CORE_SRCS} port/gw_port_posix.c)
    target_include_directories(gw_core PUBLIC include PRIVATE proto)
    target_link_libraries(gw_core PUBLIC cjson nanopb m pthread)
    target_compile_options(gw_core PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion
                                           -Wno-sign-conversion)
endif()
