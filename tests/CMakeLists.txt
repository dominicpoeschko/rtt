cmake_minimum_required(VERSION 3.28)
project(rtt_tests CXX)

include(FetchContent)

FetchContent_Declare(
    cmake_helpers
    GIT_REPOSITORY https://github.com/dominicpoeschko/cmake_helpers.git
    GIT_TAG master)

FetchContent_MakeAvailable(cmake_helpers)

include(${cmake_helpers_SOURCE_DIR}/BuildOptions.cmake)

add_subdirectory(../ ${CMAKE_CURRENT_BINARY_DIR}/rtt)

enable_testing()

set(rtt_examples_dir ${CMAKE_CURRENT_SOURCE_DIR}/../examples)

# rtt compiles for 4-byte pointers only: the host tests are built -m32 where that links (libc++ has no 32-bit package on
# Arch, so those builds skip them).
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS -m32)
set(CMAKE_REQUIRED_LINK_OPTIONS -m32)
if("${USE_STD_LIB}" STREQUAL "libc++" AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(CMAKE_REQUIRED_FLAGS "-m32 -stdlib=libc++")
    set(CMAKE_REQUIRED_LINK_OPTIONS -m32 -stdlib=libc++ -fuse-ld=lld)
endif()
check_cxx_source_compiles("#include <thread>\nint main() { std::thread t{[]{}}; t.join(); }" RTT_HAS_M32)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)

function(rtt_add_host_executable name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE rtt::rtt)
    target_compile_features(${name} PRIVATE cxx_std_23)
    target_add_default_build_options(${name} PRIVATE)
    target_compile_options(${name} PRIVATE -m32 -Wall -Wextra -Wconversion -Wsign-conversion -Werror)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        # clang's "integer" group (cmake_helpers USE_EXTRA_SANITIZERS) flags the standard library's intended unsigned
        # wraparound, see the file
        target_compile_options(
            ${name} PRIVATE -fsanitize-ignorelist=${CMAKE_CURRENT_SOURCE_DIR}/stdlib_sanitizer_ignorelist.txt)
    endif()
    target_link_options(${name} PRIVATE -m32)
endfunction()

if(RTT_HAS_M32)
    find_package(Threads REQUIRED)
    rtt_add_host_executable(rtt_test_ring ring_tests.cpp)
    target_link_libraries(rtt_test_ring PRIVATE Threads::Threads)
    add_test(NAME rtt_test_ring COMMAND rtt_test_ring)

    # The examples are the documentation: all of them compile, the ones that end are run. partial_write spins until a
    # host drains the buffer, read loops for ever.
    foreach(example static_buffers named_buffers partial_write read)
        rtt_add_host_executable(rtt_example_${example} ${rtt_examples_dir}/${example}.cpp)
    endforeach()
    add_test(NAME rtt_example_static_buffers COMMAND rtt_example_static_buffers)
    add_test(NAME rtt_example_named_buffers COMMAND rtt_example_named_buffers)

    # UBSan reports must fail the test (cmake_helpers builds with -fsanitize-recover=all)
    set_tests_properties(rtt_test_ring rtt_example_static_buffers rtt_example_named_buffers
                         PROPERTIES ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
else()
    message(STATUS "rtt tests: no -m32 toolchain, host tests skipped")
endif()

# The target side: an example cross-compiled per core, warnings as errors, and publishFence() checked in the assembly -
# a dmb on the cores SEGGER_RTT.h gives one (ARMv7E-M, ARMv8-M, ARMv8.1-M, ARMv7-A/R), none elsewhere.
find_program(RTT_ARM_GXX arm-none-eabi-g++)
find_program(RTT_CLANGXX clang++)
if(RTT_ARM_GXX)
    set(rtt_arm_cpus
        cortex-m0plus:0
        cortex-m3:0
        cortex-m4:1
        cortex-m7:1
        cortex-m23:1
        cortex-m33:1
        cortex-m55:1
        cortex-a7:1
        cortex-r5:1)
    set(rtt_arm_compilers gcc)
    if(RTT_CLANGXX)
        list(APPEND rtt_arm_compilers clang)
    endif()
    foreach(compiler ${rtt_arm_compilers})
        foreach(entry ${rtt_arm_cpus})
            string(REPLACE ":" ";" entry ${entry})
            list(GET entry 0 cpu)
            list(GET entry 1 dmb)
            add_test(
                NAME rtt_arm_${compiler}_${cpu}
                COMMAND
                    ${CMAKE_COMMAND} -DCOMPILER=${compiler} -DARM_GXX=${RTT_ARM_GXX} -DCLANGXX=${RTT_CLANGXX}
                    -DCPU=${cpu} -DEXPECT_DMB=${dmb} -DINCLUDE=${CMAKE_CURRENT_SOURCE_DIR}/../src
                    -DSOURCE=${rtt_examples_dir}/static_buffers.cpp -P ${CMAKE_CURRENT_SOURCE_DIR}/arm_check.cmake)
        endforeach()
    endforeach()
else()
    message(STATUS "rtt tests: no arm-none-eabi-g++, ARM checks skipped")
endif()
