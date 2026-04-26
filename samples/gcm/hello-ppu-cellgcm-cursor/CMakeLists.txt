# hello-ppu-cellgcm-cursor — CMake build (mirrors Makefile; both coexist until phase 7e).
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-cellgcm-cursor{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-cellgcm-cursor C)

include(ps3-self)

add_executable(hello-ppu-cellgcm-cursor source/main.c)
target_compile_features(hello-ppu-cellgcm-cursor PRIVATE c_std_11)

target_link_libraries(hello-ppu-cellgcm-cursor PRIVATE
    rsx gcm_cmd gcm_sys io sysutil rt lv2 m 
)

ps3_add_self(hello-ppu-cellgcm-cursor)
