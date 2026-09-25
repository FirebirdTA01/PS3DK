cmake_minimum_required(VERSION 3.20)

project(abi_types_regression CXX)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    # This probe checks the PPU toolchain's own type choices; a host build
    # would check the host compiler instead.
    message(STATUS "abi-types: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

include(ps3-self)

# -Werror=format turns a printf format that does not match the 32-bit
# types into a build failure, as the reference headers expect.
set(ABI_TYPES_WARNINGS -Wall -Wformat=2 -Werror=format)

add_executable(abi-types-ilp32 main.cpp)
target_compile_features(abi-types-ilp32 PRIVATE cxx_std_17)
target_compile_options(abi-types-ilp32 PRIVATE ${ABI_TYPES_WARNINGS})
ps3_add_self(abi-types-ilp32)

add_executable(abi-types-lp64 main.cpp)
target_compile_features(abi-types-lp64 PRIVATE cxx_std_17)
target_compile_options(abi-types-lp64 PRIVATE ${ABI_TYPES_WARNINGS} -mlp64)
target_link_options(abi-types-lp64 PRIVATE -mlp64)
ps3_add_self(abi-types-lp64)
