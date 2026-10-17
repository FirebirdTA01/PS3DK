cmake_minimum_required(VERSION 3.20)

project(argv_abi_regression C)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    # The subject is the lv2 process entry and the CRT; a host build tests nothing.
    message(STATUS "argv-abi: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

include(ps3-self)

# argv as main receives it from lv2, direct launch and exitspawn (main.c); the ILP32 build also exercises the CRT's
# __lv2_narrow_ea_vector directly.
add_executable(argv-abi-ilp32 main.c)
target_compile_options(argv-abi-ilp32 PRIVATE -O2 -Wall)
ps3_add_self(argv-abi-ilp32)

add_executable(argv-abi-lp64 main.c)
target_compile_options(argv-abi-lp64 PRIVATE -O2 -Wall -mlp64)
target_link_options(argv-abi-lp64 PRIVATE -mlp64)
ps3_add_self(argv-abi-lp64)
