# hello-ppu-gcm-dxt — CMake build.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-gcm-dxt C ASM)

include(ps3-self)

add_executable(hello-ppu-gcm-dxt source/main.c)
target_compile_features(hello-ppu-gcm-dxt PRIVATE c_std_11)
target_compile_options(hello-ppu-gcm-dxt PRIVATE -Wall -Wextra -Werror)
target_link_libraries(hello-ppu-gcm-dxt PRIVATE gcm_cmd gcm_sys io sysutil sysmodule rt lv2 m)

ps3_add_cg_shader_rsxcgc(hello-ppu-gcm-dxt shaders/vpshader.vcg)
ps3_add_cg_shader_rsxcgc(hello-ppu-gcm-dxt shaders/fpshader.fcg)

ps3_add_self(hello-ppu-gcm-dxt)
