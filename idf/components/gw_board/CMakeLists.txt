idf_component_register(SRCS "gw_io.c" "gw_status.c" "gw_bootbtn.c"
                       INCLUDE_DIRS "include"
                       REQUIRES gw_core
                       PRIV_REQUIRES gw_bsp esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_timer freertos)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
