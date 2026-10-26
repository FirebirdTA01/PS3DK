# lv2dbg hardware probe (debugger design study, phase 1).  See README.md.
#
# -DPS3DK_RUN_HOOK_DIR=<dir> links the ps3dk-run protocol-2 run hook from a
# checkout of spike/ps3dk-run (the directory holding hook/ and common/), so the
# network loader sees HELLO and reports the exit status.  A hardware build must
# set it; without it the probe is a plain SELF for RPCS3 or an XMB launch.
#
# -DLV2DBG_PROBE_SIM=ON also builds the failure-path simulations
# (lv2dbg-probe-sim-<case>-ilp32), for RPCS3 only.
cmake_minimum_required(VERSION 3.20)
project(lv2dbg_probe C)

include(ps3-self)

set(PS3DK_RUN_HOOK_DIR "" CACHE PATH "spike/ps3dk-run checkout providing the run hook")
option(LV2DBG_PROBE_SIM "Also build the failure-path simulations" OFF)

# Compile the hook from a copy in the build tree: objects of sources outside
# the project get object paths long enough to exceed the Windows path limit.
if(PS3DK_RUN_HOOK_DIR)
    file(COPY ${PS3DK_RUN_HOOK_DIR}/hook ${PS3DK_RUN_HOOK_DIR}/common
         DESTINATION ${CMAKE_BINARY_DIR}/run-hook)
    set(hook ${CMAKE_BINARY_DIR}/run-hook)
endif()

function(lv2dbg_probe t abi sim)
    add_executable(${t} source/main.c)
    target_compile_features(${t} PRIVATE c_std_11)
    target_compile_options(${t} PRIVATE -O2 -Wall -Wextra)
    target_compile_definitions(${t} PRIVATE PROBE_SIM=${sim})
    if(abi STREQUAL "lp64")
        target_compile_options(${t} PRIVATE -mlp64)
        target_link_options(${t} PRIVATE -mlp64)
    endif()
    target_link_libraries(${t} PRIVATE lv2dbg_stub sysmodule_stub c_stub sysutil)
    if(PS3DK_RUN_HOOK_DIR)
        target_sources(${t} PRIVATE
            ${hook}/hook/ps3dk_run_hook.c
            ${hook}/common/prn_net.c
            ${hook}/common/prn_sha256.c
            ${hook}/common/prn_hmac.c)
        target_include_directories(${t} PRIVATE ${hook}/hook)
        target_compile_definitions(${t} PRIVATE PROBE_RUN_HOOK=1)
        target_link_libraries(${t} PRIVATE net)
    endif()
    target_link_libraries(${t} PRIVATE rt lv2 m)
    ps3_add_self(${t})
endfunction()

foreach(abi ilp32 lp64)
    lv2dbg_probe(lv2dbg-probe-${abi} ${abi} 0)
endforeach()

if(LV2DBG_PROBE_SIM)
    set(cases ok log-open log-write dabr-active cleanup-fail log-exists)
    set(n 1)
    foreach(c ${cases})
        lv2dbg_probe(lv2dbg-probe-sim-${c}-ilp32 ilp32 ${n})
        math(EXPR n "${n} + 1")
    endforeach()
endif()
