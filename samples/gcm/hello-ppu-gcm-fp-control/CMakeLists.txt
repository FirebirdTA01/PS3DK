cmake_minimum_required(VERSION 3.20)
project(hello-ppu-gcm-fp-control C ASM)
include(ps3-self)
add_executable(hello-ppu-gcm-fp-control source/main.c)
target_compile_features(hello-ppu-gcm-fp-control PRIVATE c_std_11)
target_compile_options(hello-ppu-gcm-fp-control PRIVATE -Wall -Wextra)
target_link_libraries(hello-ppu-gcm-fp-control PRIVATE gcm_cmd rt lv2)
# Reuse the existing color-pass-through shader; no synthetic CGprogram.
ps3_add_cg_shader_rsxcgc(hello-ppu-gcm-fp-control
    ../hello-ppu-cellgcm-triangle/shaders/fpshader.fcg)
ps3_add_self(hello-ppu-gcm-fp-control)
