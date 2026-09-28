# hello-spurs-task2 - Taskset2 / Task2 sample (PPU + SPU task image).
#
# The SPU side is a Spurs Task (not a regular SPU thread): linked
# with `-nostartfiles` against our spurs_task.ld script + libspurs_task.a.
# Its return value becomes the task's exit code, which the PPU joins via
# Taskset2::joinTask2.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(hello-spurs-task2 CXX ASM)

include(ps3-self)

add_executable(hello-spurs-task2 source/main.cpp)
target_compile_features(hello-spurs-task2 PRIVATE cxx_std_17)

target_link_libraries(hello-spurs-task2 PRIVATE
    spurs_stub c_stub sysmodule_stub sysutil rt lv2 m
)

# Custom SPU build: -nostartfiles + spurs_task.ld + libspurs_task.
ps3_add_spu_image(hello-spurs-task2
    NAME spu_task
    SOURCES spu/source/spu_task_main.c
    CFLAGS -O2 -Wall -Wextra -Wno-cpp
    NOSTARTFILES
    FREESTANDING
    LDSCRIPT ${PS3DK}/spu/ldscripts/spurs_task.ld
    LIBS spurs_task)

ps3_add_self(hello-spurs-task2)
