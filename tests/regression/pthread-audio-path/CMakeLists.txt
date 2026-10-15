cmake_minimum_required(VERSION 3.20)
project(pthread_audio_path_regression C)

option(PS3TC_REGRESSION_HOST_STUB "Host cannot validate Lv-2 pthreads" OFF)
if(PS3TC_REGRESSION_HOST_STUB)
    return()
endif()

include(ps3-self)

# One probe, both ABIs: the ILP32 target uses the toolchain default, the
# LP64 target opts in with -mlp64 (ps3_add_self then post-links it with
# sprxlinker --lp64).
foreach(abi ilp32 lp64)
    add_executable(pthread-audio-path-${abi} main.c)
    target_compile_features(pthread-audio-path-${abi} PRIVATE c_std_11)
    target_compile_options(pthread-audio-path-${abi} PRIVATE -Wall -Wextra -Werror=implicit-function-declaration)
    if(abi STREQUAL "lp64")
        target_compile_options(pthread-audio-path-${abi} PRIVATE -mlp64)
        target_link_options(pthread-audio-path-${abi} PRIVATE -mlp64)
    endif()
    ps3_add_self(pthread-audio-path-${abi})
endforeach()
