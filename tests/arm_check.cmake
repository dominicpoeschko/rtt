# cmake -DCOMPILER=gcc|clang -DARM_GXX=... [-DCLANGXX=...] -DCPU=cortex-m33 -DEXPECT_DMB=0|1 -DINCLUDE=<rtt/src>
# -DSOURCE=<file.cpp> -P arm_check.cmake
#
# Compiles SOURCE to assembly for CPU with warnings as errors and checks that it has a dmb exactly when EXPECT_DMB says.
# clang uses arm-none-eabi-gcc's newlib and libstdc++ (the multilib directories for this very CPU).

set(flags
    -mcpu=${CPU}
    -mthumb
    -std=c++23
    -O2
    -fno-exceptions
    -Wall
    -Wextra
    -Wconversion
    -Wsign-conversion
    -Werror
    -I${INCLUDE})

if(COMPILER STREQUAL "gcc")
    set(cxx ${ARM_GXX})
elseif(COMPILER STREQUAL "clang")
    set(cxx ${CLANGXX})
    execute_process(
        COMMAND ${ARM_GXX} -print-sysroot
        OUTPUT_VARIABLE sysroot
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(
        COMMAND ${ARM_GXX} -mcpu=${CPU} -mthumb -xc++ -E -v -
        INPUT_FILE /dev/null
        OUTPUT_QUIET
        ERROR_VARIABLE search)
    string(REGEX MATCH "#include <...> search starts here:\n(.*)End of search list" _ "${search}")
    string(REGEX REPLACE "\n +" ";" dirs "${CMAKE_MATCH_1}")
    list(APPEND flags --target=arm-none-eabi --sysroot=${sysroot} -nostdinc++)
    foreach(dir ${dirs})
        string(STRIP "${dir}" dir)
        if(dir)
            list(APPEND flags -isystem ${dir})
        endif()
    endforeach()
else()
    message(FATAL_ERROR "COMPILER is gcc or clang, not '${COMPILER}'")
endif()

execute_process(
    COMMAND ${cxx} ${flags} -S ${SOURCE} -o -
    OUTPUT_VARIABLE asm
    ERROR_VARIABLE errors
    RESULT_VARIABLE result)
if(NOT result EQUAL 0 OR errors)
    message(FATAL_ERROR "${COMPILER} ${CPU}: compile failed or warned (${result}):\n${errors}")
endif()

string(REGEX MATCHALL "[ \t]dmb" dmbs "${asm}")
list(LENGTH dmbs count)
if(EXPECT_DMB AND count EQUAL 0)
    message(FATAL_ERROR "${COMPILER} ${CPU}: no dmb before the offset is published")
elseif(NOT EXPECT_DMB AND NOT count EQUAL 0)
    message(FATAL_ERROR "${COMPILER} ${CPU}: ${count} dmb where the core needs none")
endif()
message(STATUS "${COMPILER} ${CPU}: ${count} dmb, as expected")
