idf_component_register(SRCS "gw_web.c" "gw_mdns.c" "gw_webcert.c" "gw_webtls.c"
                       INCLUDE_DIRS "include"
                       REQUIRES gw_core
                       PRIV_REQUIRES gw_ap gw_board gw_bus gw_edge gw_fw gw_log gw_net gw_storage esp_http_server esp_timer
                                     esp_system heap esp_https_server mbedtls lwip
                       EMBED_TXTFILES "www/index.html" "www/claim.html" "www/portal.html")
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/gw_warnings.cmake)
gw_target_options(${COMPONENT_LIB})
# Issue #291: the dynamic TLS buffers of ESP-IDF free the RX buffer that holds a ClientHello
# without TLS 1.3, before the TLS 1.2 code of mbedTLS parses it; gw_webtls.c keeps it
if(CONFIG_MBEDTLS_DYNAMIC_BUFFER)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_mbedtls_free_rx_buffer"
                                                     "-u __wrap_esp_mbedtls_free_rx_buffer")
endif()
