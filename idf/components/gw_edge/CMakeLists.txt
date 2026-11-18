idf_component_register(SRCS "gw_edge.c"
                       INCLUDE_DIRS "include"
                       REQUIRES gw_core
                       PRIV_REQUIRES gw_board gw_bus gw_fw gw_net gw_storage esp_timer esp_system freertos lwip
                                     mbedtls tcp_transport)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/gw_warnings.cmake)
gw_target_options(${COMPONENT_LIB})
