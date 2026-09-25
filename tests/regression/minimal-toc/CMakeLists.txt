cmake_minimum_required(VERSION 3.20)

project(minimal_toc_regression C)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    # -mminimal-toc is a PPU code-generation mode; a host build tests nothing.
    message(STATUS "minimal-toc: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

include(ps3-self)

# Every TOC access in this program goes through the minimal TOC: the base is
# loaded into r30 from its slot in the ELF TOC (GCC patch 0043), so a wrong
# slot size or load reads every global through a wrong address.
foreach(abi ilp32 lp64)
    add_executable(minimal-toc-${abi} main.c other.c)
    target_compile_options(minimal-toc-${abi} PRIVATE -O2 -mminimal-toc -Wall -Werror)
    if(abi STREQUAL "lp64")
        target_compile_options(minimal-toc-${abi} PRIVATE -mlp64)
        target_link_options(minimal-toc-${abi} PRIVATE -mlp64)
    endif()
    ps3_add_self(minimal-toc-${abi})
endforeach()
