idf_component_register(SRCS "gw_mbtcp.c"
                       INCLUDE_DIRS "include"
                       REQUIRES gw_core
                       PRIV_REQUIRES gw_ap gw_board gw_bus gw_net freertos lwip log)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
