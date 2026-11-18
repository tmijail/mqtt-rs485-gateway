# Issue #227: the compiler options of the own components (gw_* and main), in one place
# (docs/reference/compiler-warnings-and-stack.md). Components of ESP-IDF and managed components keep their own options.
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
# Issue #226: clang (the fuzz targets, test/fuzz) has no "=strict" level. Its -Wcast-align warns
# on each cast to a type with a larger alignment, also on x86-64, as the =strict level of GCC.
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    list(TRANSFORM GW_WARNING_FLAGS REPLACE "^-Wcast-align=strict$" "-Wcast-align")
endif()

# The host build of gw_core and the host tests (test/host): the same, plus -Wundef. The firmware
# can not use -Wundef: the headers of ESP-IDF evaluate undefined macros in "#if" (more than
# 3000 warnings), and ESP-IDF leaves a disabled bool option undefined, so "#if CONFIG_GW_X" is
# correct code. The host test check_kconfig_names finds a misspelled CONFIG_GW_ name instead.
set(GW_HOST_WARNING_FLAGS ${GW_WARNING_FLAGS} -Wundef)

# The stack data of each function, for tools/stack_report.py: <object>.su (-fstack-usage) and the
# call graph <object>.ci (-fcallgraph-info=su). They do not change the generated code.
set(GW_STACK_FLAGS -fstack-usage -fcallgraph-info=su)

# The largest stack frame of one own function in the firmware (with -Werror: an error). 2048 B is
# half of the stack of rs485_bus (4096 B); the largest frame today is 1664 B
# (gw_storage_load_config). Decision of the product owner, 2026-11-18 (#227). A frame that the
# compiler can not bound (alloca) also fails. Only the firmware: frames on the host differ.
set(GW_STACK_LIMIT_FLAGS -Wstack-usage=2048)

# Issue #230: the GCC static analyzer for the own components only, in a separate build:
#   idf.py -B build-analyzer -D GW_ANALYZER=ON build
# CONFIG_COMPILER_STATIC_ANALYZER of ESP-IDF adds -fanalyzer to all components, ESP-IDF too.
# With -Werror, a result of the analyzer stops the build. The default (OFF) and thus the
# release build do not change. docs/reference/compiler-warnings-and-stack.md

function(gw_target_options target)
    target_compile_options(${target} PRIVATE ${GW_WARNING_FLAGS} ${GW_STACK_FLAGS}
                           ${GW_STACK_LIMIT_FLAGS})
    if(GW_ANALYZER)
        target_compile_options(${target} PRIVATE -fanalyzer)
    endif()
    # No ccache (IDF_CCACHE_ENABLE) for the own components: at a hit, ccache 4.9 gives the object
    # and the .su file again, but not the .ci file, and the stack report then has no call graph.
    set_property(TARGET ${target} PROPERTY RULE_LAUNCH_COMPILE "")
endfunction()
