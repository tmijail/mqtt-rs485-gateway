idf_component_register(SRCS "gw_fw.c"
                       INCLUDE_DIRS "include"
                       REQUIRES gw_core
                       PRIV_REQUIRES app_update esp_https_ota esp_http_client esp_timer
                                     bootloader_support mbedtls freertos log gw_bus gw_storage)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
# Issue #102: the capabilities block after esp_app_desc_t, kept by the linker
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u gw_app_caps")
