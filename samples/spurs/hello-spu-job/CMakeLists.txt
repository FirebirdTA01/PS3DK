# hello-spu-job - end-to-end cellSpursJobMain2 entry-point bring-up.
#
# Compiles a real SPU job binary (spu/source/main.c) using
# spurs_job.ld + libspurs_job for the layout/trampoline, embeds it via
# bin2s, and drives it through a Spurs2 job chain on the PPU side.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
cmake_minimum_required(VERSION 3.20)
project(hello-spu-job C CXX ASM)

include(ps3-self)

add_executable(hello-spu-job source/main.cpp)
target_compile_features(hello-spu-job PRIVATE cxx_std_17)

target_link_libraries(hello-spu-job PRIVATE
    spurs_stub c_stub sysmodule_stub sysutil rt lv2 m
)

# The SPU job binary uses the spurs-job entry-point convention:
#   - NOSTARTFILES (no SPU crt0 - libspurs_job's _start trampoline owns
#     the entry point)
#   - LDSCRIPT spurs_job.ld (anchors _start in .before_text at LS 0)
#   - LIBS spurs_job (the trampoline + cellSpursJobInitialize)
ps3_add_spu_image(hello-spu-job
    NAME hello_spu_job
    SOURCES spu/source/main.c
    NOSTARTFILES
    JOBBIN
    # SPURS_JOB build posture: -fpic comes from the macro's non-FREESTANDING
    # default; -mspurs-job sets ELF e_flags=1 (= "SPURS Job 2.0 w/o CRT")
    # via our patched SPU GCC + binutils; -Wl,-q (--emit-relocs) keeps
    # .rela* sections in the linked ELF for any runtime fix-up the
    # dispatcher applies.
    CFLAGS -mspurs-job
    LDFLAGS -mspurs-job -Wl,-q
    LDSCRIPT ${PS3DK}/spu/ldscripts/spurs_job.ld
    LIBS spurs_job)

ps3_add_self(hello-spu-job)
