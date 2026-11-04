idf_component_register(SRCS "gw_storage.c"
                       INCLUDE_DIRS "include"
                       REQUIRES gw_core
                       PRIV_REQUIRES esp_partition nvs_flash vfs
                       EMBED_TXTFILES "default_config.json")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
