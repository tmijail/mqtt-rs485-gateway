# Issue #167: the option lists of the web UI's configuration form are the full value sets of
# the configuration parser. A <select> without the current value shows its first option, and a
# save writes that option back (a CDAB tag became "big"). Fails unless each list in index.html
# has the same values, in the same order, as its array in gw_config.c.
#   cmake -DCONFIG_C=<gw_config.c> -DINDEX_HTML=<index.html> -P check_web_lists.cmake
file(READ "${CONFIG_C}" c_text)
file(READ "${INDEX_HTML}" html)

function(quoted_items text out)
    string(REGEX MATCHALL "\"[^\"]*\"" items "${text}")
    set(${out} "${items}" PARENT_SCOPE)
endfunction()

set(failed "")
foreach(pair "PAR;k_parity_names" "AREA;k_area_names" "TYPE;k_type_names" "WO;k_word_names"
             "NAMING;k_naming_names")
    list(GET pair 0 js)
    list(GET pair 1 c)
    string(REGEX MATCH "${c}\\[\\] = {([^}]*)}" m "${c_text}")
    if(NOT m)
        message(FATAL_ERROR "${CONFIG_C}: array ${c} not found")
    endif()
    quoted_items("${CMAKE_MATCH_1}" c_items)
    string(REGEX MATCH "[^A-Za-z_]${js}=\\[([^]]*)\\]" m "${html}")
    if(NOT m)
        message(FATAL_ERROR "${INDEX_HTML}: list ${js}=[...] not found")
    endif()
    quoted_items("${CMAKE_MATCH_1}" js_items)
    if(NOT c_items STREQUAL js_items)
        string(APPEND failed "\n  ${js} = ${js_items}\n  ${c} = ${c_items}")
    else()
        list(LENGTH js_items n)
        message(STATUS "${js} = ${c} (${n} values)")
    endif()
endforeach()
if(failed)
    message(FATAL_ERROR "the form lists of index.html differ from gw_config.c (issue #167):${failed}")
endif()
