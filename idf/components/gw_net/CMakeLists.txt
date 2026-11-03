idf_component_register(SRCS "gw_net.c"
                       INCLUDE_DIRS "include"
                       REQUIRES gw_core esp_netif
                       PRIV_REQUIRES esp_eth esp_driver_spi esp_driver_gpio esp_hw_support lwip)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
