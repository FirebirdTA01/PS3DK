cmake_minimum_required(VERSION 3.20)

project(altivec_strict_std_regression CXX)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    # This probe checks the PPU compiler's AltiVec keywords; a host build
    # would check the host compiler instead.
    message(STATUS "altivec-strict-std: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

include(ps3-self)

# CXX_EXTENSIONS OFF makes this -std=c++17, not -std=gnu++17: the strict mode
# the reference samples use (they pass -std=c++98), where upstream GCC drops
# the context-sensitive vector/pixel/bool keywords.
foreach(abi ilp32 lp64)
    add_executable(altivec-strict-std-${abi} main.cpp)
    set_target_properties(altivec-strict-std-${abi} PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_compile_options(altivec-strict-std-${abi} PRIVATE -Wall -Werror)
    if(abi STREQUAL "lp64")
        target_compile_options(altivec-strict-std-${abi} PRIVATE -mlp64)
        target_link_options(altivec-strict-std-${abi} PRIVATE -mlp64)
    endif()
    ps3_add_self(altivec-strict-std-${abi})
endforeach()
