idf_component_register(SRCS "gw_mbtcp.c"
                       INCLUDE_DIRS "include"
                       REQUIRES gw_core
                       PRIV_REQUIRES gw_ap gw_board gw_bus gw_net gw_storage freertos lwip log)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/gw_warnings.cmake)
gw_target_options(${COMPONENT_LIB})
