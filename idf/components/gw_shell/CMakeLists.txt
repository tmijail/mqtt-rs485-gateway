idf_component_register(SRCS "gw_shell.c"
                       INCLUDE_DIRS "include"
                       REQUIRES gw_core
                       PRIV_REQUIRES gw_board gw_bus gw_edge gw_log gw_net gw_storage gw_web console esp_timer freertos heap lwip)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
