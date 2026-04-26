# hello-ppu-cellgcm-sysmenu — CMake build (mirrors Makefile; both coexist until phase 7e).
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-cellgcm-sysmenu C ASM)

include(ps3-self)

add_executable(hello-ppu-cellgcm-sysmenu source/main.c)
target_compile_features(hello-ppu-cellgcm-sysmenu PRIVATE c_std_11)

target_link_libraries(hello-ppu-cellgcm-sysmenu PRIVATE
    dbgfont gcm_cmd gcm_sys io sysutil sysmodule rt lv2 m
)

ps3_add_cg_shader(hello-ppu-cellgcm-sysmenu shaders/vpshader.vcg)
ps3_add_cg_shader(hello-ppu-cellgcm-sysmenu shaders/fpshader.fcg)

ps3_add_self(hello-ppu-cellgcm-sysmenu)
