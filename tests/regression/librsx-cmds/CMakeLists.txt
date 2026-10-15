cmake_minimum_required(VERSION 3.20)

project(librsx_cmds_regression C)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    # The probe is about what the PPU rsx command functions emit; a host
    # build has no rsx library to test.
    message(STATUS "librsx-cmds: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

include(ps3-self)

# The same probe against both copies of the PSL1GHT-derived rsx functions
# (librsx, and the legacy copy inside libgcm_cmd), in both ABIs.
foreach(lib rsx gcm_cmd)
    foreach(abi ilp32 lp64)
        set(t librsx-cmds-${lib}-${abi})
        add_executable(${t} main.c)
        target_compile_options(${t} PRIVATE -Wall -Wextra -Werror)
        target_link_libraries(${t} PRIVATE ${lib} gcm_sys rt lv2)
        if(abi STREQUAL "lp64")
            target_compile_options(${t} PRIVATE -mlp64)
            target_link_options(${t} PRIVATE -mlp64)
        endif()
        ps3_add_self(${t})
    endforeach()
endforeach()
