# hello-spu — minimal PPU+SPU smoke test (CMake build).
#
# The SPU side (spu/source/main.c) compiles to an SPU ELF, which gets
# embedded into the PPU executable via bin2s under the symbols
# main_elf / main_elf_end / main_elf_size — the PPU code uses them
# with sys_spu_image_import.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-spu{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-spu C ASM)

include(ps3-self)

add_executable(hello-spu source/main.c)
target_compile_features(hello-spu PRIVATE c_std_11)

target_link_libraries(hello-spu PRIVATE
    rt lv2 m
)

# SPU side — single-source standalone task that the PPU spawns via
# sys_spu_thread_initialize.  Embedded by bin2s under the
# `hello_spu_elf*` symbol set (matches the current Makefile's
# hello_spu_bin* by convention rename).
ps3_add_spu_image(hello-spu
    NAME hello_spu
    SOURCES spu/source/main.c
    LIBS sputhread simdmath)

ps3_add_self(hello-spu)
