# hello-ppu-cellgcm-state-tour — self-checking tour of the native cellGcm state calls.
#
# Renders each state group into an offscreen color+depth surface in MAIN
# memory (PPU-readable), probes fixed pixels, and prints
#   STATE_TOUR <group> PASS|FAIL expected=0x... got=0x...
# per check. Exits 0 only when every check passed.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-cellgcm-state-tour C ASM)

include(ps3-self)

add_executable(hello-ppu-cellgcm-state-tour source/main.c)
target_compile_features(hello-ppu-cellgcm-state-tour PRIVATE c_std_11)

target_link_libraries(hello-ppu-cellgcm-state-tour PRIVATE
    gcm_cmd gcm_sys io sysutil sysutil_stub sysmodule rt lv2 m
)

ps3_add_cg_shader_rsxcgc(hello-ppu-cellgcm-state-tour shaders/vpshader.vcg)
ps3_add_cg_shader_rsxcgc(hello-ppu-cellgcm-state-tour shaders/fpshader.fcg)

ps3_add_self(hello-ppu-cellgcm-state-tour)

ps3_add_pkg(hello-ppu-cellgcm-state-tour
    APPID     "GCMX00022"
    CONTENTID "UP0001-GCMX00022_00-PS3DKSAMPLES0000")
