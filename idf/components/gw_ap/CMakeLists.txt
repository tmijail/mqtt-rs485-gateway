idf_component_register(SRCS "gw_ap.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_netif gw_core
                       PRIV_REQUIRES gw_net esp_wifi esp_timer esp_hw_support freertos)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/gw_warnings.cmake)
gw_target_options(${COMPONENT_LIB})
