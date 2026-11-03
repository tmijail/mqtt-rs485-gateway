idf_component_register(SRCS "gw_shell.c"
                       INCLUDE_DIRS "include"
                       REQUIRES gw_core
                       PRIV_REQUIRES gw_board console esp_timer heap)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
