# hello-ppu-cellgcm-quad — CMake build.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-cellgcm-quad C ASM)

include(ps3-self)

add_executable(hello-ppu-cellgcm-quad source/main.c)
target_compile_features(hello-ppu-cellgcm-quad PRIVATE c_std_11)

target_link_libraries(hello-ppu-cellgcm-quad PRIVATE
    gcm_cmd gcm_sys io sysutil sysmodule rt lv2 m
)

# Still on cgcomp — blocked on rsx-cg-compiler shader-feature work.
# vpshader.vcg builds float4(in_position.x, in_position.y, 0, 1) from a
# float2 input; the rsx-cg-compiler IR doesn't yet lower that pattern
# (skeleton-stage failure on float2-based vec4 ctor / swizzle-assign).
# Migrate once that gap closes.  See samples/gcm/hello-ppu-cellgcm-triangle
# for the canonical migration template.
ps3_add_cg_shader(hello-ppu-cellgcm-quad shaders/vpshader.vcg)
ps3_add_cg_shader(hello-ppu-cellgcm-quad shaders/fpshader.fcg)

ps3_add_self(hello-ppu-cellgcm-quad)
