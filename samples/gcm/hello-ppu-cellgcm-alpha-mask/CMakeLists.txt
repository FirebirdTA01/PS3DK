# hello-ppu-cellgcm-alpha-mask — CMake build.
#
# rsx-cg-compiler validation: FP alpha-mask if-else lowering plus
# implicit varying-binding inference, both end-to-end through
# cellGcmCg* + cellGcmSetFragmentProgram.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-cellgcm-alpha-mask C ASM)

include(ps3-self)

add_executable(hello-ppu-cellgcm-alpha-mask source/main.c)
target_compile_features(hello-ppu-cellgcm-alpha-mask PRIVATE c_std_11)

target_link_libraries(hello-ppu-cellgcm-alpha-mask PRIVATE
    gcm_cmd gcm_sys io sysutil sysmodule rt lv2 m
)

ps3_add_cg_shader_rsxcgc(hello-ppu-cellgcm-alpha-mask shaders/vpshader.vcg)
ps3_add_cg_shader_rsxcgc(hello-ppu-cellgcm-alpha-mask shaders/fpshader.fcg)

ps3_add_self(hello-ppu-cellgcm-alpha-mask)
