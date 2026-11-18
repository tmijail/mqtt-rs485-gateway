idf_component_register(SRCS "gw_fw.c"
                       INCLUDE_DIRS "include"
                       REQUIRES gw_core
                       PRIV_REQUIRES app_update esp_https_ota esp_http_client esp_timer
                                     bootloader_support mbedtls freertos log gw_bus gw_storage)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/gw_warnings.cmake)
gw_target_options(${COMPONENT_LIB})
# Issue #102: the capabilities block after esp_app_desc_t, kept by the linker
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u gw_app_caps")
# Issue #112 (QEMU build only): blocking IPC calls go through gw_fw.c, which reports one made to
# core 1 while gw_fw keeps it parked
if(CONFIG_GW_FW_PARK_OTHER_CORE)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_ipc_call"
                                                     "-Wl,--wrap=esp_ipc_call_blocking")
endif()

# Issues #144 item 3 and #148: the capabilities block declares the LittleFS disk version and the
# NVS page format that this image writes. Both come from the sources that the build uses (the
# managed littlefs component, the nvs_flash component of IDF); the build fails when they are not
# found, so that a library update cannot change them without the block.
idf_component_get_property(gw_lfs_dir joltwallet__littlefs COMPONENT_DIR)
file(STRINGS "${gw_lfs_dir}/src/littlefs/lfs.h" gw_lfs_line
     REGEX "^#define LFS_DISK_VERSION[ \t]+0x[0-9a-fA-F]+")
idf_build_get_property(gw_idf_path IDF_PATH)
file(STRINGS "${gw_idf_path}/components/nvs_flash/private_include/nvs_constants.h" gw_nvs_line
     REGEX "^#define NVS_CONST_NVS_VERSION[ \t]+0x[0-9a-fA-F]+")
if(NOT gw_lfs_line OR NOT gw_nvs_line)
    message(FATAL_ERROR "gw_fw: LFS_DISK_VERSION or NVS_CONST_NVS_VERSION not found "
                        "(issue #148: the capabilities block needs them)")
endif()
string(REGEX REPLACE ".*(0x[0-9a-fA-F]+).*" "\\1" gw_lfs_version "${gw_lfs_line}")
string(REGEX REPLACE ".*(0x[0-9a-fA-F]+).*" "\\1" gw_nvs_version "${gw_nvs_line}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE GW_LFS_DISK_VERSION=${gw_lfs_version}u
                                                    GW_NVS_PAGE_VERSION=${gw_nvs_version}u)
