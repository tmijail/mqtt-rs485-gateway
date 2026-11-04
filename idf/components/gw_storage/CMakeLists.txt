idf_component_register(SRCS "gw_storage.c" "gw_sfcard.c"
                       INCLUDE_DIRS "include"
                       REQUIRES gw_core
                       PRIV_REQUIRES esp_partition nvs_flash vfs heap esp_hw_support mbedtls gw_bsp fatfs
                                     esp_driver_sdmmc sdmmc
                       EMBED_TXTFILES "default_config.json")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
