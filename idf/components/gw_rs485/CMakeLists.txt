idf_component_register(SRCS "gw_rs485.c"
                       INCLUDE_DIRS "include"
                       REQUIRES gw_core
                       PRIV_REQUIRES gw_bsp esp_driver_uart esp_hal_uart esp_timer esp_system esp_rom)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/gw_warnings.cmake)
gw_target_options(${COMPONENT_LIB})
# Mask the RS485 UART's interrupts on every restart (see gw_rs485.c, __wrap_esp_restart_noos).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_restart_noos")
