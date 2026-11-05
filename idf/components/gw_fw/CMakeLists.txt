idf_component_register(SRCS "gw_fw.c"
                       INCLUDE_DIRS "include"
                       REQUIRES gw_core
                       PRIV_REQUIRES app_update esp_https_ota esp_http_client esp_timer
                                     bootloader_support mbedtls freertos log gw_bus gw_storage)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
# Issue #102: the capabilities block after esp_app_desc_t, kept by the linker
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u gw_app_caps")
# Issue #112 (QEMU build only): blocking IPC calls go through gw_fw.c, which reports one made to
# core 1 while gw_fw keeps it parked
if(CONFIG_GW_FW_PARK_OTHER_CORE)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_ipc_call"
                                                     "-Wl,--wrap=esp_ipc_call_blocking")
endif()
