cmake_minimum_required(VERSION 3.20)

project(ilp32_doloop_regression C)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    # The defect is PPU counted-loop codegen; a host build tests nothing.
    message(STATUS "ilp32-doloop: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

include(ps3-self)

# doloop.c is its own unit so digits() keeps a trip count the compiler cannot
# fold, built from a zero-extended load (GCC patch 0050).
add_executable(ilp32-doloop main.c doloop.c)
target_compile_options(ilp32-doloop PRIVATE -O2 -Wall)
ps3_add_self(ilp32-doloop)
