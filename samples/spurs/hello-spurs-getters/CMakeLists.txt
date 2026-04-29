# hello-spurs-getters - validate the libspurs SPU module-runtime
# getters (cellSpursGetSpursAddress / GetCurrentSpuId / GetTagId /
# GetWorkloadId / GetSpuCount) by reading them from inside an SPU
# task and DMA-putting the values back to the PPU.

cmake_minimum_required(VERSION 3.20)
project(hello-spurs-getters CXX ASM)

include(ps3-self)

add_executable(hello-spurs-getters source/main.cpp)
target_compile_features(hello-spurs-getters PRIVATE cxx_std_17)

target_link_libraries(hello-spurs-getters PRIVATE
    spurs_stub c_stub sysmodule_stub sysutil rt lv2 m
)

ps3_add_spu_image(hello-spurs-getters
    NAME spu_getters
    SOURCES spu/source/spu_getters_main.c
    CFLAGS -O2 -Wall -Wextra -Wno-cpp
    NOSTARTFILES
    FREESTANDING
    LDSCRIPT ${PS3DK}/spu/ldscripts/spurs_task.ld
    LIBS spurs_task)

ps3_add_self(hello-spurs-getters)
