# hello-ppu-prof-auto-verdicts — known-answer phases for AUTO frame verdicts.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake \
#     -DPS3_PROF_AUTO=ON \
#     -DPS3_PROF_AUTO_ROOT=<sdk-root>
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-prof-auto-verdicts C ASM)

include(ps3-self)

add_executable(hello-ppu-prof-auto-verdicts source/main.c)
target_compile_features(hello-ppu-prof-auto-verdicts PRIVATE c_std_11)

target_link_libraries(hello-ppu-prof-auto-verdicts PRIVATE
    gcm_cmd gcm_sys io sysutil sysutil_stub sysmodule rt lv2 m
)

ps3_add_cg_shader_rsxcgc(hello-ppu-prof-auto-verdicts shaders/vpshader.vcg)
ps3_add_cg_shader_rsxcgc(hello-ppu-prof-auto-verdicts shaders/fpshader.fcg)

ps3_add_self(hello-ppu-prof-auto-verdicts)

ps3_add_pkg(hello-ppu-prof-auto-verdicts
    APPID     "GCMX00023"
    CONTENTID "UP0001-GCMX00023_00-PS3DKSAMPLES0000")
