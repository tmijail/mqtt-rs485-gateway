idf_component_register(SRCS "gw_edge.c"
                       INCLUDE_DIRS "include"
                       REQUIRES gw_core
                       PRIV_REQUIRES gw_board gw_bus gw_net gw_storage esp_timer esp_system freertos lwip
                                     mbedtls)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
