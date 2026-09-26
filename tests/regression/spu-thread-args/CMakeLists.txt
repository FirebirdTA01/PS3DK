cmake_minimum_required(VERSION 3.20)
project(spu_thread_args_regression C ASM)
option(PS3TC_REGRESSION_HOST_STUB "Host cannot run SPU code" OFF)
if(PS3TC_REGRESSION_HOST_STUB)
    return()
endif()
include(ps3-self)
# One PPU harness: the check runs in the SPU thread's entry, which has no
# PPU data-model axis (and ps3_add_spu_image keys its outputs on NAME alone).
add_executable(spu-thread-args main.c)
target_compile_features(spu-thread-args PRIVATE c_std_11)
target_compile_options(spu-thread-args PRIVATE -Wall -Wextra -Werror=implicit-function-declaration)
target_link_libraries(spu-thread-args PRIVATE lv2)
ps3_add_spu_image(spu-thread-args
    NAME spu
    SOURCES spu/main.c
    CFLAGS -O2 -Wall -Wextra -Werror)
ps3_add_self(spu-thread-args)
