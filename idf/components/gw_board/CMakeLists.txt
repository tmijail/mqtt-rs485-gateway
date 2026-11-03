idf_component_register(SRCS "gw_io.c" "gw_status.c"
                       INCLUDE_DIRS "include"
                       REQUIRES gw_core
                       PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_uart esp_timer
                                     freertos vfs)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
