cmake_minimum_required(VERSION 3.20)
project(hello-ppu-gcm-report-ring C ASM)
include(ps3-self)
add_executable(hello-ppu-gcm-report-ring source/main.c)
target_compile_features(hello-ppu-gcm-report-ring PRIVATE c_std_11)
target_compile_options(hello-ppu-gcm-report-ring PRIVATE -O2 -Wall -Wextra -Werror)
target_link_libraries(hello-ppu-gcm-report-ring PRIVATE
    gcm_cmd gcm_sys io sysutil sysutil_stub sysmodule rt lv2 m)
# Reuse the proven state-tour sources, without shader or compiler changes.
ps3_add_cg_shader_rsxcgc(hello-ppu-gcm-report-ring ../hello-ppu-cellgcm-state-tour/shaders/vpshader.vcg)
ps3_add_cg_shader_rsxcgc(hello-ppu-gcm-report-ring ../hello-ppu-cellgcm-state-tour/shaders/fpshader.fcg)
ps3_add_self(hello-ppu-gcm-report-ring)
ps3_add_pkg(hello-ppu-gcm-report-ring
    APPID "GCMR00001" CONTENTID "UP0001-GCMR00001_00-PS3DKSAMPLES0000")
