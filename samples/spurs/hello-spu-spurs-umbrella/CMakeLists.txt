# hello-spu-spurs-umbrella — regression gate for the slim SPU spurs.h
# umbrella and the four new sub-headers shipped in the cell/spurs
# surface (task_exit_code, policy_module, lv2_event_queue, lfqueue).
#
# The SPU side includes <cell/spurs.h> as its primary spurs include
# (not individual sub-headers) and exercises each new type surface.
# The PPU side attaches/detaches an LV2 event queue to exercise the
# lv2_event_queue.h prototypes.
#
# Build:
#   cmake -S . -B build
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(hello-spu-spurs-umbrella CXX ASM)

include(ps3-self)

add_executable(hello-spu-spurs-umbrella source/main.cpp)
target_compile_features(hello-spu-spurs-umbrella PRIVATE cxx_std_17)

target_link_libraries(hello-spu-spurs-umbrella PRIVATE
    spurs_stub c_stub sysmodule_stub sysutil rt lv2 m
)

# SPU task linked with -nostartfiles + spurs_task.ld + libspurs_task.a
ps3_add_spu_image(hello-spu-spurs-umbrella
    NAME spu_task
    SOURCES spu/source/spu_task_main.c
    CFLAGS -O2 -Wall -Wextra -Wno-cpp
    NOSTARTFILES
    FREESTANDING
    LDSCRIPT ${PS3DK}/spu/ldscripts/spurs_task.ld
    LIBS spurs_task)

ps3_add_self(hello-spu-spurs-umbrella)
