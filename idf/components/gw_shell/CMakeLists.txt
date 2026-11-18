idf_component_register(SRCS "gw_shell.c"
                       INCLUDE_DIRS "include"
                       REQUIRES gw_core
                       PRIV_REQUIRES gw_ap gw_board gw_bus gw_edge gw_fw gw_log gw_mbtcp gw_net gw_storage gw_web console esp_timer freertos heap lwip spi_flash)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/gw_warnings.cmake)
gw_target_options(${COMPONENT_LIB})
