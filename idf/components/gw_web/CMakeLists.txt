idf_component_register(SRCS "gw_web.c" "gw_mdns.c" "gw_webcert.c"
                       INCLUDE_DIRS "include"
                       REQUIRES gw_core
                       PRIV_REQUIRES gw_board gw_edge gw_fw gw_log gw_net gw_storage esp_http_server esp_timer
                                     esp_system heap esp_https_server mbedtls lwip
                       EMBED_TXTFILES "www/index.html")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
