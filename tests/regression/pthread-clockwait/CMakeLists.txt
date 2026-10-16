cmake_minimum_required(VERSION 3.20)

project(pthread_clockwait_regression C)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    # This probe is about librt's clock-selecting waits and sysconf on Lv-2;
    # a host build would test the host libc.
    message(STATUS "pthread-clockwait: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

include(ps3-self)

# One probe, both ABIs: the ILP32 target uses the toolchain default, the
# LP64 target opts in with -mlp64 (ps3_add_self then post-links it with
# sprxlinker --lp64).
add_executable(pthread-clockwait-ilp32 main.c)
ps3_add_self(pthread-clockwait-ilp32)

add_executable(pthread-clockwait-lp64 main.c)
target_compile_options(pthread-clockwait-lp64 PRIVATE -mlp64)
target_link_options(pthread-clockwait-lp64 PRIVATE -mlp64)
ps3_add_self(pthread-clockwait-lp64)
