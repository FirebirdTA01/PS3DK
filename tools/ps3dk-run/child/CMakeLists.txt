# ps3dk-run test child: links the run hook and exercises the loader acceptance list.
cmake_minimum_required(VERSION 3.20)
project(ps3dk-run-child C CXX)

include(ps3-self)

add_executable(ps3dk-run-child
    source/main.cpp
    ../hook/ps3dk_run_hook.c
    ../common/prn_net.c
    ../common/prn_sha256.c
    ../common/prn_hmac.c)
target_compile_options(ps3dk-run-child PRIVATE -Wall -Wextra)
# Test-only fault injection in the hook (see PS3DK_RUN_HOOK_FAULT in ../hook/ps3dk_run_hook.c); 0 = off, the pinned build.
set(PS3DK_RUN_HOOK_FAULT 0 CACHE STRING "ps3dk-run hook fault injection: 0 off, 1 watchdog, 2 sender, 3 HELLO refused, 4 HELLO_OK rejected, 5 watchdog wins + relaunch fails, 6 watchdog wins, 7 as 5 with a failed join and a late publish")
if(NOT PS3DK_RUN_HOOK_FAULT STREQUAL "0")
    target_compile_definitions(ps3dk-run-child PRIVATE PS3DK_RUN_HOOK_FAULT=${PS3DK_RUN_HOOK_FAULT})
endif()
target_link_libraries(ps3dk-run-child PRIVATE net sysmodule rt lv2 m)

ps3_add_self(ps3dk-run-child)
