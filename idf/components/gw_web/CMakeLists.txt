idf_component_register(SRCS "gw_web.c" "gw_mdns.c" "gw_webcert.c"
                       INCLUDE_DIRS "include"
                       REQUIRES gw_core
                       PRIV_REQUIRES gw_ap gw_board gw_bus gw_edge gw_fw gw_log gw_net gw_storage esp_http_server esp_timer
                                     esp_system heap esp_https_server mbedtls lwip
                       EMBED_TXTFILES "www/index.html" "www/claim.html")
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/gw_warnings.cmake)
gw_target_options(${COMPONENT_LIB})
