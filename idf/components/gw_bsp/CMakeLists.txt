idf_component_register(SRCS "gw_bsp.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_spi esp_driver_uart)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
