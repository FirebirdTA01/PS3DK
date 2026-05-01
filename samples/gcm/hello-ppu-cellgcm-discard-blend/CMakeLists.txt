# hello-ppu-cellgcm-discard-blend — CMake build.
#
# rsx-cg-compiler validation: compound discard (LogicalAnd + MULXC),
# simple discard (FX12 SLT + KIL), and post-discard VecConstructTexMul
# output mixing, all end-to-end through cellGcmCg*.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-cellgcm-discard-blend C ASM)

include(ps3-self)

add_executable(hello-ppu-cellgcm-discard-blend source/main.c)
target_compile_features(hello-ppu-cellgcm-discard-blend PRIVATE c_std_11)

target_link_libraries(hello-ppu-cellgcm-discard-blend PRIVATE
    gcm_cmd gcm_sys io sysutil sysmodule rt lv2 m
)

ps3_add_cg_shader_rsxcgc(hello-ppu-cellgcm-discard-blend shaders/vpshader.vcg)
ps3_add_cg_shader_rsxcgc(hello-ppu-cellgcm-discard-blend shaders/fpshader.fcg)

ps3_add_self(hello-ppu-cellgcm-discard-blend)
