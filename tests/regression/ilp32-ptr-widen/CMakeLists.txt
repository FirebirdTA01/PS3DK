cmake_minimum_required(VERSION 3.20)

project(ilp32_ptr_widen_regression C CXX)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    # The defect is PPU ILP32 pointer widening; a host build tests nothing.
    message(STATUS "ilp32-ptr-widen: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

include(ps3-self)

# widen.c / widen.cpp hold the conversions and values.c the inputs, each its
# own unit (no LTO) so no constant reaches a conversion (GCC patch 0029).
add_executable(ilp32-ptr-widen main.c values.c widen.c widen.cpp)
target_compile_options(ilp32-ptr-widen PRIVATE -O2 -Wall -fno-lto)
# The direct pointer-to-64-bit casts are the subject of the test.
set_source_files_properties(widen.c PROPERTIES COMPILE_OPTIONS -Wno-pointer-to-int-cast)
ps3_add_self(ilp32-ptr-widen)
