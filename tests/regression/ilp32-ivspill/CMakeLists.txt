cmake_minimum_required(VERSION 3.20)

project(ilp32_ivspill_regression CXX)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    # The defect is PPU address generation; a host build tests nothing.
    message(STATUS "ilp32-ivspill: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

include(ps3-self)

# ivspill.cpp is its own unit so evolve() keeps the loop shape that made the
# IV optimizer spill a biased address IV (GCC patch 0049).  Run it under the
# PPU interpreter: RPCS3's LLVM recompiler truncates a bad address to 32 bits
# and the row would pass on a broken compiler.
add_executable(ilp32-ivspill main.cpp ivspill.cpp)
target_compile_options(ilp32-ivspill PRIVATE -O3 -Wall)
target_link_libraries(ilp32-ivspill PRIVATE m)
ps3_add_self(ilp32-ivspill)
