idf_component_register(SRCS "gw_rs485.c"
                       INCLUDE_DIRS "include"
                       REQUIRES gw_core
                       PRIV_REQUIRES esp_driver_uart esp_timer esp_system esp_rom)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
