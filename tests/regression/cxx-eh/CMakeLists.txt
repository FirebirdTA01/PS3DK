cmake_minimum_required(VERSION 3.20)

project(cxx_eh_regression CXX)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    # This probe is about PPU unwinding and per-thread exception state;
    # a host build would test the host's runtime instead.
    message(STATUS "cxx-eh: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

include(ps3-self)

# One probe, both ABIs: the ILP32 target uses the toolchain default, the
# LP64 target opts in with -mlp64 (ps3_add_self then post-links it with
# sprxlinker --lp64).
add_executable(cxx-eh-ilp32 main.cpp)
target_compile_features(cxx-eh-ilp32 PRIVATE cxx_std_17)
ps3_add_self(cxx-eh-ilp32)

add_executable(cxx-eh-lp64 main.cpp)
target_compile_features(cxx-eh-lp64 PRIVATE cxx_std_17)
target_compile_options(cxx-eh-lp64 PRIVATE -mlp64)
target_link_options(cxx-eh-lp64 PRIVATE -mlp64)
ps3_add_self(cxx-eh-lp64)
