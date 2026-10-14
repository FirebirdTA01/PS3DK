cmake_minimum_required(VERSION 3.20)

project(ssp_regression C)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    # The probe exercises the PPU compiler's stack protector sequences.
    message(STATUS "ssp: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

include(ps3-self)

# One probe per protector level and data model (GCC patch 0067).  The
# probe defines __stack_chk_guard and __stack_chk_fail itself.
foreach(level stack-protector stack-protector-strong stack-protector-all)
    foreach(abi ilp32 lp64)
        set(t ssp-${level}-${abi})
        string(REPLACE "stack-protector" "sp" t "${t}")
        add_executable(${t} main.c)
        target_compile_options(${t} PRIVATE -O2 -Wall -f${level})
        if(abi STREQUAL "lp64")
            target_compile_options(${t} PRIVATE -mlp64)
            target_link_options(${t} PRIVATE -mlp64)
        endif()
        ps3_add_self(${t})
    endforeach()
endforeach()
