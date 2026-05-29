# hello-ppu-gcm-basic — CMake build.
#
# Compiles vpshader.vcg / fpshader.fcg via rsx-cg-compiler (CgBinaryProgram
# container output), embeds them via bin2s, and links against the cellGcm
# forwarders + RSX runtime.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-gcm-basic{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-gcm-basic C ASM)

include(ps3-self)

add_executable(hello-ppu-gcm-basic source/main.c)
target_compile_features(hello-ppu-gcm-basic PRIVATE c_std_11)

target_link_libraries(hello-ppu-gcm-basic PRIVATE
    gcm_cmd gcm_sys io sysutil sysmodule rt lv2 m
)

ps3_add_cg_shader_rsxcgc(hello-ppu-gcm-basic shaders/vpshader.vcg)
ps3_add_cg_shader_rsxcgc(hello-ppu-gcm-basic shaders/fpshader.fcg)

ps3_add_self(hello-ppu-gcm-basic)
