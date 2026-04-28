# hello-spurs-jq - cell/spurs/job_queue*.h header validation.
#
# Compile-and-link probe that exercises every entry point declared in
# cell/spurs/job_queue.h, job_queue_port.h, job_queue_port2.h, and
# job_queue_semaphore.h against the existing libspurs_jq_stub.a NIDs.
# Doesn't run end-to-end (the SPU runtime is a later phase) - purpose
# is to surface header / struct ABI errors before the runtime work.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(hello-spurs-jq C CXX ASM)

include(ps3-self)

add_executable(hello-spurs-jq source/main.cpp)
target_compile_features(hello-spurs-jq PRIVATE cxx_std_17)

target_link_libraries(hello-spurs-jq PRIVATE
    spurs_jq_stub spurs_stub c_stub sysmodule_stub sysutil rt lv2 m
)

# SPU side - JOBBIN_WRAP: SPURS JOB-QUEUE workloads need the
# reference jobbin2 wrapper format (0x100-byte ELF prefix in front
# of the LS image).  ps3_add_spu_image runs wine spu_elf-to-ppu_obj.exe
# to produce a .ppu.o exposing _binary_<NAME>_jobbin2_jobheader and
# the embedded blob.  PPU code copies the jobheader symbol directly
# into the descriptor (no manual descriptor build).
ps3_add_spu_image(hello-spurs-jq
    NAME hello_spurs_jq
    SOURCES spu/source/main.c
    NOSTARTFILES
    JOBBIN_WRAP
    CFLAGS -mspurs-job-initialize
    LDFLAGS -mspurs-job-initialize -Wl,-q -Wl,--gc-sections
    LDSCRIPT ${PS3DK}/spu/ldscripts/spurs_job.ld
    LIBS spurs_jq)

ps3_add_self(hello-spurs-jq)
