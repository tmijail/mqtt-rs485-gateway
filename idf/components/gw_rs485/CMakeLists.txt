idf_component_register(SRCS "gw_rs485.c"
                       INCLUDE_DIRS "include"
                       REQUIRES gw_core
                       PRIV_REQUIRES gw_bsp esp_driver_uart esp_hal_uart esp_timer esp_system esp_rom)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
# Mask the RS485 UART's interrupts on every restart (see gw_rs485.c, __wrap_esp_restart_noos).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_restart_noos")
