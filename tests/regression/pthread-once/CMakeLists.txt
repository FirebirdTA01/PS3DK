cmake_minimum_required(VERSION 3.20)

project(pthread_once_regression CXX)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    # This probe is about librt's pthread_once on Lv-2 threads, with a C++
    # exception unwinding through it; a host build would test the host libc.
    message(STATUS "pthread-once: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

include(ps3-self)

# One probe, both ABIs: the ILP32 target uses the toolchain default, the
# LP64 target opts in with -mlp64 (ps3_add_self then post-links it with
# sprxlinker --lp64).
add_executable(pthread-once-ilp32 main.cpp)
target_compile_features(pthread-once-ilp32 PRIVATE cxx_std_17)
ps3_add_self(pthread-once-ilp32)

add_executable(pthread-once-lp64 main.cpp)
target_compile_features(pthread-once-lp64 PRIVATE cxx_std_17)
target_compile_options(pthread-once-lp64 PRIVATE -mlp64)
target_link_options(pthread-once-lp64 PRIVATE -mlp64)
ps3_add_self(pthread-once-lp64)
