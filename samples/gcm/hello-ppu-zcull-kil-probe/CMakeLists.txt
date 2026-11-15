# hello-ppu-zcull-kil-probe — characterisation probe for RSX Zcull and KIL interaction.
#
# Measures RSX ticks, ZPASS pixel count, and ZCULL_STATS across eight cases
# (visible vs occluded, heavy nokil vs heavy kil, zcull on vs off).
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-zcull-kil-probe C ASM)

include(ps3-self)

add_executable(hello-ppu-zcull-kil-probe source/main.c)
target_compile_features(hello-ppu-zcull-kil-probe PRIVATE c_std_11)

target_link_libraries(hello-ppu-zcull-kil-probe PRIVATE
    gcm_cmd gcm_sys io sysutil sysutil_stub sysmodule rt lv2 m
)

ps3_add_cg_shader_rsxcgc(hello-ppu-zcull-kil-probe shaders/vp_quad.vcg)
ps3_add_cg_shader_rsxcgc(hello-ppu-zcull-kil-probe shaders/fp_flat.fcg)
ps3_add_cg_shader_rsxcgc(hello-ppu-zcull-kil-probe shaders/fp_heavy_nokil.fcg)
ps3_add_cg_shader_rsxcgc(hello-ppu-zcull-kil-probe shaders/fp_heavy_kil.fcg)

ps3_add_self(hello-ppu-zcull-kil-probe)

ps3_add_pkg(hello-ppu-zcull-kil-probe
    APPID     "GCMX00023"
    CONTENTID "UP0001-GCMX00023_00-PS3DKSAMPLES0000")
