cmake_minimum_required(VERSION 3.20)

project(cxx_iostream_startup_regression CXX)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    # This probe is about the PPU runtime start-up order (newlib locks vs
    # early constructors); a host build would test the host runtime instead.
    message(STATUS "cxx-iostream-startup: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

include(ps3-self)

# One probe, both ABIs: the ILP32 target uses the toolchain default, the
# LP64 target opts in with -mlp64 (ps3_add_self then post-links it with
# sprxlinker --lp64).
add_executable(cxx-iostream-startup-ilp32 main.cpp)
target_compile_features(cxx-iostream-startup-ilp32 PRIVATE cxx_std_17)
ps3_add_self(cxx-iostream-startup-ilp32)

add_executable(cxx-iostream-startup-lp64 main.cpp)
target_compile_features(cxx-iostream-startup-lp64 PRIVATE cxx_std_17)
target_compile_options(cxx-iostream-startup-lp64 PRIVATE -mlp64)
target_link_options(cxx-iostream-startup-lp64 PRIVATE -mlp64)
ps3_add_self(cxx-iostream-startup-lp64)
