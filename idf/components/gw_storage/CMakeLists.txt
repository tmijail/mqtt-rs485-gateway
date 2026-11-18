idf_component_register(SRCS "gw_storage.c" "gw_sfcard.c" "gw_coredump.c" "gw_pemfile.c"
                       INCLUDE_DIRS "include"
                       REQUIRES gw_core
                       PRIV_REQUIRES esp_partition app_update nvs_flash vfs heap esp_hw_support mbedtls gw_bsp fatfs
                                     esp_driver_sdmmc sdmmc esp_timer freertos espcoredump
                       EMBED_TXTFILES "default_config.json")
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/gw_warnings.cmake)
gw_target_options(${COMPONENT_LIB})
