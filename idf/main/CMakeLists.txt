idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES gw_core gw_board gw_bsp gw_bus gw_edge gw_net gw_rs485 gw_storage
                                     esp_app_format esp_event esp_netif esp_system freertos)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)

# LittleFS image of GW_FS_DIR, flashed to the "storage" partition by `idf.py flash`.
# Put a config.json there to override the embedded default (CFG-2).
littlefs_create_partition_image(storage ${GW_FS_DIR} FLASH_IN_PROJECT)
