cmake_minimum_required(VERSION 3.20)
project(spu_fiber_context_regression C ASM)
option(PS3TC_REGRESSION_HOST_STUB "Host cannot run SPU code" OFF)
if(PS3TC_REGRESSION_HOST_STUB)
    return()
endif()
include(ps3-self)
# One PPU harness: everything under test runs on the SPU, which has no PPU
# data-model axis (and ps3_add_spu_image keys its outputs on NAME alone).
add_executable(spu-fiber-context main.c)
target_compile_features(spu-fiber-context PRIVATE c_std_11)
target_compile_options(spu-fiber-context PRIVATE -Wall -Wextra -Werror=implicit-function-declaration)
target_link_libraries(spu-fiber-context PRIVATE lv2)
ps3_add_spu_image(spu-fiber-context
    NAME spu
    SOURCES spu/main.c
    CFLAGS -O2 -Wall -Wextra -Werror
    LIBS fiber)
ps3_add_self(spu-fiber-context)
