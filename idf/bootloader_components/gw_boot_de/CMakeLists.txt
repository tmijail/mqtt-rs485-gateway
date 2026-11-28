# Issue #300: a component of the second-stage bootloader only. The bootloader build takes each
# component of <project>/bootloader_components (components/bootloader/subproject/CMakeLists.txt
# of ESP-IDF); the application build does not see this folder.
#
# The pin and its polarity come from gw_bsp_pins.h of gw_bsp, the one source of the board facts.
# Only that header: gw_bsp.h needs the drivers, which the bootloader does not have.
idf_component_register(SRCS "gw_boot_de.c"
                       PRIV_INCLUDE_DIRS "../../components/gw_bsp/include"
                       PRIV_REQUIRES log esp_hal_gpio soc)
# The compiler options of the own components (-Werror, -fanalyzer with GW_ANALYZER, which
# idf/CMakeLists.txt gives to the bootloader build too)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/gw_warnings.cmake)
gw_target_options(${COMPONENT_LIB})
