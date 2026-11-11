idf_component_register(SRCS "gw_log.c"
                       INCLUDE_DIRS "include"
                       REQUIRES gw_core
                       PRIV_REQUIRES freertos lwip log heap esp_hw_support gw_storage)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
