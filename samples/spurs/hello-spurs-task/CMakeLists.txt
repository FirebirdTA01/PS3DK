# hello-spurs-task — Spurs Taskset / Task framework smoke test.
#
# The SPU side is a Spurs Task (not a regular SPU thread): linked
# with `-nostartfiles` against our spurs_task.ld script + libspurs_task.a
# so the resulting ELF is the Task entry-point body the framework
# expects.  Embedded into the PPU ELF via bin2s under the
# spu_task_elf symbol set.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(hello-spurs-task CXX ASM)

include(ps3-self)

add_executable(hello-spurs-task source/main.cpp)
target_compile_features(hello-spurs-task PRIVATE cxx_std_17)

target_link_libraries(hello-spurs-task PRIVATE
    spurs_stub c_stub sysmodule_stub sysutil rt lv2 m
)

# Custom SPU build: -nostartfiles + spurs_task.ld + libspurs_task.
# Mirrors the SPU recipe in the parallel Makefile.
ps3_add_spu_image(hello-spurs-task
    NAME spu_task
    SOURCES spu/source/spu_task_main.c
    CFLAGS -O2 -Wall -Wextra -Wno-cpp
    NOSTARTFILES
    FREESTANDING
    LDSCRIPT ${PS3DK}/spu/ldscripts/spurs_task.ld
    LIBS spurs_task)

ps3_add_self(hello-spurs-task)
