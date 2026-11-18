# Issue #227: the compiler options of the own components (gw_* and main), in one place
# (idf/README.md §3.8). Components of ESP-IDF and managed components keep their own options.
#
#   include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/gw_warnings.cmake)   # main: ../cmake/
#   gw_target_options(${COMPONENT_LIB})
#
# gw_core adds -Wconversion -Wno-sign-conversion, in the firmware build and in the host build.
set(GW_WARNING_FLAGS
    -Wall -Wextra -Werror
    -Wshadow             # a local hides another variable
    -Wformat=2           # printf formats that are not literals, and format security
    -Wnull-dereference   # a path that the optimizer proves to dereference NULL (-O2, -Os)
    -Wvla                # no stack size that the code can not see at compile time
    -Wstrict-prototypes  # "void f()" accepts any arguments; write "void f(void)"
    -Wcast-align=strict) # a cast to a type with a larger alignment (Xtensa traps on it); =strict
                         # warns on the host too, where unaligned access is permitted

# The host build of gw_core and the host tests (test/host): the same, plus -Wundef. The firmware
# can not use -Wundef: the headers of ESP-IDF evaluate undefined macros in "#if" (more than
# 3000 warnings), and ESP-IDF leaves a disabled bool option undefined, so "#if CONFIG_GW_X" is
# correct code. The host test check_kconfig_names finds a misspelled CONFIG_GW_ name instead.
set(GW_HOST_WARNING_FLAGS ${GW_WARNING_FLAGS} -Wundef)

# The stack data of each function, for tools/stack_report.py: <object>.su (-fstack-usage) and the
# call graph <object>.ci (-fcallgraph-info=su). They do not change the generated code.
set(GW_STACK_FLAGS -fstack-usage -fcallgraph-info=su)

function(gw_target_options target)
    target_compile_options(${target} PRIVATE ${GW_WARNING_FLAGS} ${GW_STACK_FLAGS})
endfunction()
