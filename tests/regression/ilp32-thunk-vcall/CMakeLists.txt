cmake_minimum_required(VERSION 3.20)

project(ilp32_thunk_vcall_regression CXX)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    # The defect is PPU virtual-thunk codegen; a host build tests nothing.
    message(STATUS "ilp32-thunk-vcall: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

include(ps3-self)

# -fno-inline keeps the thunk on the rs6000 assembly-thunk path and keeps
# Derived::probe reading its incoming `this` register (GCC patch 0056).
add_executable(ilp32-thunk-vcall main.cpp thunk.cpp)
target_compile_options(ilp32-thunk-vcall PRIVATE -O2 -fno-inline -Wall)
ps3_add_self(ilp32-thunk-vcall)
