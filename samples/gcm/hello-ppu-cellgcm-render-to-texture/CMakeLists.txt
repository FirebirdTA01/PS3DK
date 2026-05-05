# hello-ppu-cellgcm-render-to-texture — CMake build.

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-cellgcm-render-to-texture C)

include(ps3-self)

add_executable(hello-ppu-cellgcm-render-to-texture source/main.c)
target_compile_features(hello-ppu-cellgcm-render-to-texture PRIVATE c_std_11)

target_link_libraries(hello-ppu-cellgcm-render-to-texture PRIVATE
    rsx gcm_cmd gcm_sys io sysutil rt lv2
)

ps3_add_self(hello-ppu-cellgcm-render-to-texture)
