idf_component_register(SRCS "gw_io.c" "gw_status.c" "gw_bootbtn.c" "gw_buzzer.c" "gw_temp.c"
                       INCLUDE_DIRS "include"
                       REQUIRES gw_core
                       PRIV_REQUIRES gw_bsp gw_storage esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_driver_tsens esp_timer
                                     esp_driver_pcnt esp_system freertos)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/gw_warnings.cmake)
gw_target_options(${COMPONENT_LIB})
