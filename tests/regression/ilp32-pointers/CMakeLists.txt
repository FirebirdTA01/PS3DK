cmake_minimum_required(VERSION 3.20)

project(ilp32_pointers_regression C CXX)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    # The defect class is PPU ILP32 address formation; a host build tests nothing.
    message(STATUS "ilp32-pointers: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

include(ps3-self)

# pointers.c is its own unit so the address forms reach the code generator
# instead of folding into main (GCC patch 0059).  Judge it under the PPU
# interpreter: RPCS3's LLVM recompiler truncates a bad address to 32 bits.
add_executable(ilp32-pointers main.c pointers.c classes.cpp)
target_compile_options(ilp32-pointers PRIVATE -O2 -Wall -maltivec)
# The flags a game engine built the reproducer with; without them GCC 12.4
# did not emit the bad shape for this unit.
set_source_files_properties(classes.cpp PROPERTIES COMPILE_OPTIONS
    "-std=c++17;-mstrict-align;-fmodulo-sched;-ffunction-sections")
ps3_add_self(ilp32-pointers)
