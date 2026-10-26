# lv2dbg hardware probe (debugger design study, phase 1).  See README.md.
#
# -DPS3DK_RUN_HOOK_DIR=<dir> links the ps3dk-run protocol-2 run hook from a
# checkout of spike/ps3dk-run (the directory holding hook/ and common/), so the
# network loader sees HELLO and reports the exit status.  A hardware build must
# set it; without it the probe is a plain SELF for RPCS3 or an XMB launch.
cmake_minimum_required(VERSION 3.20)
project(lv2dbg_probe C)

include(ps3-self)

set(PS3DK_RUN_HOOK_DIR "" CACHE PATH "spike/ps3dk-run checkout providing the run hook")

foreach(abi ilp32 lp64)
    set(t lv2dbg-probe-${abi})
    add_executable(${t} source/main.c)
    target_compile_features(${t} PRIVATE c_std_11)
    target_compile_options(${t} PRIVATE -O2 -Wall -Wextra)
    if(abi STREQUAL "lp64")
        target_compile_options(${t} PRIVATE -mlp64)
        target_link_options(${t} PRIVATE -mlp64)
    endif()
    target_link_libraries(${t} PRIVATE lv2dbg_stub sysmodule_stub c_stub sysutil)
    if(PS3DK_RUN_HOOK_DIR)
        target_sources(${t} PRIVATE
            ${PS3DK_RUN_HOOK_DIR}/hook/ps3dk_run_hook.c
            ${PS3DK_RUN_HOOK_DIR}/common/prn_net.c
            ${PS3DK_RUN_HOOK_DIR}/common/prn_sha256.c
            ${PS3DK_RUN_HOOK_DIR}/common/prn_hmac.c)
        target_include_directories(${t} PRIVATE ${PS3DK_RUN_HOOK_DIR}/hook)
        target_compile_definitions(${t} PRIVATE PROBE_RUN_HOOK=1)
        target_link_libraries(${t} PRIVATE net)
    endif()
    target_link_libraries(${t} PRIVATE rt lv2 m)
    ps3_add_self(${t})
endforeach()
