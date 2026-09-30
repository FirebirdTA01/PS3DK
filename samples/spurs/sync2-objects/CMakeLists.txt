# sync2-objects - cellSync2 mutex, semaphore, condition variable and queue
# shared by SPURS tasks, a SPURS job and PPU threads.
#
# The PPU calls go to the libsync2 system module (sync2_stub); the SPU task
# and job link libsync2.a.  The job is built in -mspurs-job driver mode.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(sync2-objects C CXX ASM)

include(ps3-self)

add_executable(sync2-objects source/main.cpp)
target_compile_features(sync2-objects PRIVATE cxx_std_17)
target_compile_options(sync2-objects PRIVATE -Wall -Wextra)
target_link_libraries(sync2-objects PRIVATE
    sync2_stub spurs_stub c_stub sysmodule_stub sysutil rt lv2 m
)

ps3_add_spu_image(sync2-objects
    NAME sync2_task
    SOURCES spu/task.c
    CFLAGS -O2 -Wall -Wextra -Wno-cpp
    NOSTARTFILES
    FREESTANDING
    LDSCRIPT ${PS3DK}/spu/ldscripts/spurs_task.ld
    LIBS spurs_task sync2 spurs_jq spurs fiber)

ps3_add_spu_image(sync2-objects
    NAME sync2_job
    SOURCES spu/job.c
    JOBBIN
    CFLAGS -O2 -Wall -Wextra -mspurs-job
    LDFLAGS -mspurs-job -Wl,-q
    LIBS sync2 spurs_jq spurs fiber)

ps3_add_self(sync2-objects)
