# hello-spurs-semaphore — CellSpursSemaphore P/V validation.
#
# Two SPU tasks in one Class-0 Taskset share a 128-byte
# CellSpursSemaphore in main memory.  Task 0 is the producer (V's
# eight times); task 1 is the consumer (P's eight times, incrementing
# a PPU-visible counter after each successful acquire).  PPU spins
# until the counter reaches 8, then shuts the taskset down.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
cmake_minimum_required(VERSION 3.20)
project(hello-spurs-semaphore CXX ASM)

include(ps3-self)

add_executable(hello-spurs-semaphore source/main.cpp)
target_compile_features(hello-spurs-semaphore PRIVATE cxx_std_17)

target_link_libraries(hello-spurs-semaphore PRIVATE
    spurs_stub c_stub sysmodule_stub sysutil rt lv2 m
)

ps3_add_spu_image(hello-spurs-semaphore
    NAME spu_task
    SOURCES spu/source/spu_task_main.c
    CFLAGS -O2 -Wall -Wextra -Wno-cpp
    NOSTARTFILES
    FREESTANDING
    LDSCRIPT ${PS3DK}/spu/ldscripts/spurs_task.ld
    LIBS spurs_task)

ps3_add_self(hello-spurs-semaphore)
