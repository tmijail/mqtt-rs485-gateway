idf_component_register(SRCS "gw_io.c" "gw_status.c" "gw_bootbtn.c" "gw_buzzer.c" "gw_temp.c"
                       INCLUDE_DIRS "include"
                       REQUIRES gw_core
                       PRIV_REQUIRES gw_bsp gw_storage esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_driver_tsens esp_timer
                                     esp_system freertos)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
