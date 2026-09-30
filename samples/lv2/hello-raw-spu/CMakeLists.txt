# hello-raw-spu - a raw SPU driven through its problem-state registers,
# with the reference-SDK names of <sys/raw_spu.h>: sys_raw_spu_create /
# image_load, SPU_In_MBox / SPU_NPC / SPU_RunCntl / SPU_Out_MBox /
# SPU_Status through sys_raw_spu_mmio_*, and LS_BASE_ADDR for the local
# store.  Prints HELLO_RAW_SPU OK when the SPU's answer and stop code
# come back.
#
# Build:
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
cmake_minimum_required(VERSION 3.20)
project(hello-raw-spu C ASM)

include(ps3-self)

add_executable(hello-raw-spu source/main.c)
target_compile_features(hello-raw-spu PRIVATE c_std_11)
target_link_libraries(hello-raw-spu PRIVATE c_stub rt lv2 m)

ps3_add_spu_image(hello-raw-spu
    NAME spu_raw
    SOURCES spu/raw.c)

ps3_add_self(hello-raw-spu)
