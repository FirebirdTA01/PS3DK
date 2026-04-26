# hello-ppu-cellgcm-loops — CMake build.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-cellgcm-loops C ASM)

include(ps3-self)

add_executable(hello-ppu-cellgcm-loops source/main.c)
target_compile_features(hello-ppu-cellgcm-loops PRIVATE c_std_11)

target_link_libraries(hello-ppu-cellgcm-loops PRIVATE
    gcm_cmd gcm_sys io sysutil sysmodule rt lv2 m
)

ps3_add_cg_shader(hello-ppu-cellgcm-loops shaders/vpshader.vcg)
ps3_add_cg_shader(hello-ppu-cellgcm-loops shaders/fpshader.fcg)

ps3_add_self(hello-ppu-cellgcm-loops)
