cmake_minimum_required(VERSION 3.20)

project(ilp32_pointers_regression C)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    # The defect class is PPU ILP32 address formation; a host build tests nothing.
    message(STATUS "ilp32-pointers: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

include(ps3-self)

# pointers.c is its own unit so the address forms reach the code generator
# instead of folding into main (GCC patch 0051).  Judge it under the PPU
# interpreter: RPCS3's LLVM recompiler truncates a bad address to 32 bits.
add_executable(ilp32-pointers main.c pointers.c)
target_compile_options(ilp32-pointers PRIVATE -O2 -Wall -maltivec)
ps3_add_self(ilp32-pointers)
