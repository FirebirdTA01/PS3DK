# hello-ppu-cellgcm-flip — CMake build (mirrors Makefile; both coexist until phase 7e).
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-cellgcm-flip{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-cellgcm-flip C)

include(ps3-self)

add_executable(hello-ppu-cellgcm-flip source/main.c)
target_compile_features(hello-ppu-cellgcm-flip PRIVATE c_std_11)

target_link_libraries(hello-ppu-cellgcm-flip PRIVATE
    gcm_cmd gcm_sys io sysutil rt lv2 
)

ps3_add_self(hello-ppu-cellgcm-flip)
