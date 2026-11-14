idf_component_register(SRCS "gw_ap.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_netif gw_core
                       PRIV_REQUIRES esp_wifi esp_timer esp_hw_support freertos)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
