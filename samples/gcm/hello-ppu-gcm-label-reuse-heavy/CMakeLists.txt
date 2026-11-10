# The heavy form of hello-ppu-gcm-label-reuse: the same source built with LABEL_REUSE_HEAVY (a 1024x1024 texture,
# 256 draws over a 512x512 target) and an 8-tap fragment program, so the RSX is still reading the arena long after
# its front end passes a command label.
cmake_minimum_required(VERSION 3.20)
project(hello-ppu-gcm-label-reuse-heavy C ASM)
include(ps3-self)
add_executable(hello-ppu-gcm-label-reuse-heavy ../hello-ppu-gcm-label-reuse/source/main.c)
target_compile_features(hello-ppu-gcm-label-reuse-heavy PRIVATE c_std_11)
target_compile_options(hello-ppu-gcm-label-reuse-heavy PRIVATE -O2 -Wall -Wextra -Werror)
target_compile_definitions(hello-ppu-gcm-label-reuse-heavy PRIVATE LABEL_REUSE_HEAVY=1)
target_link_libraries(hello-ppu-gcm-label-reuse-heavy PRIVATE gcm_cmd gcm_sys sysutil rt lv2 m)
ps3_add_cg_shader_rsxcgc(hello-ppu-gcm-label-reuse-heavy shaders/arena_vp.vcg)
ps3_add_cg_shader_rsxcgc(hello-ppu-gcm-label-reuse-heavy shaders/arena_fp.fcg)
ps3_add_self(hello-ppu-gcm-label-reuse-heavy)
ps3_add_pkg(hello-ppu-gcm-label-reuse-heavy
    APPID "GCML00002" CONTENTID "UP0001-GCML00002_00-PS3DKSAMPLES0000")
