# hello-ppu-cellgcm-laneelision — CMake build.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-cellgcm-laneelision C ASM)

include(ps3-self)

add_executable(hello-ppu-cellgcm-laneelision source/main.c)
target_compile_features(hello-ppu-cellgcm-laneelision PRIVATE c_std_11)

target_link_libraries(hello-ppu-cellgcm-laneelision PRIVATE
    gcm_cmd gcm_sys io sysutil sysmodule rt lv2 m
)

ps3_add_cg_shader_rsxcgc(hello-ppu-cellgcm-laneelision shaders/vpshader.vcg)
ps3_add_cg_shader_rsxcgc(hello-ppu-cellgcm-laneelision shaders/fpshader.fcg)

ps3_add_self(hello-ppu-cellgcm-laneelision)
