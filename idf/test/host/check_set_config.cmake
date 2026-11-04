# Issue #58: fails unless every esp_mqtt_set_config() call in the firmware sources is the one
# inside gw_edge's guarded wrapper (which refuses a configuration that still carries
# network.if_name, leaked by esp-mqtt 1.1.0 on every call).
#   cmake -DSRC=<gw_edge.c> -P check_set_config.cmake
file(READ "${SRC}" text)
# a call has an argument list; comments that name the function do not count
string(REGEX MATCHALL "esp_mqtt_set_config[ \t]*\\([ \t]*[A-Za-z_]" calls "${text}")
list(LENGTH calls n)
string(FIND "${text}" "s_mqtt_cfg.network.if_name != NULL ||" guarded)
if(NOT n EQUAL 1)
    message(FATAL_ERROR "${SRC}: ${n} calls to esp_mqtt_set_config(); only the guarded "
                        "set_config() wrapper may call it (issue #58)")
endif()
if(guarded EQUAL -1)
    message(FATAL_ERROR "${SRC}: the if_name guard of set_config() is gone (issue #58)")
endif()
get_filename_component(src_real "${SRC}" REALPATH)
get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/../.." REALPATH)
file(GLOB_RECURSE others "${root}/components/*.c" "${root}/main/*.c")
foreach(f ${others})
    get_filename_component(f_real "${f}" REALPATH)
    if(NOT f_real STREQUAL src_real)
        file(READ "${f}" t)
        string(REGEX MATCH "esp_mqtt_set_config[ \t]*\\(" at "${t}")
        if(at)
            message(FATAL_ERROR "${f} calls esp_mqtt_set_config(): use gw_edge's guarded wrapper "
                                "(issue #58)")
        endif()
    endif()
endforeach()
message(STATUS "esp_mqtt_set_config only in the guarded wrapper")
