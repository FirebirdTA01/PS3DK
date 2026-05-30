cmake_minimum_required(VERSION 3.20)
project(hello-ppu-camera C)

include(ps3-self)

add_executable(hello-ppu-camera source/main.c)
target_compile_features(hello-ppu-camera PRIVATE c_std_11)

target_link_libraries(hello-ppu-camera PRIVATE
    camera_stub sysmodule_stub c_stub sysutil rt lv2
)

ps3_add_self(hello-ppu-camera)
