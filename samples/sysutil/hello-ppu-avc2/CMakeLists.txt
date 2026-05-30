cmake_minimum_required(VERSION 3.20)
project(hello-ppu-avc2 C)

include(ps3-self)

add_executable(hello-ppu-avc2 source/main.c)
target_compile_features(hello-ppu-avc2 PRIVATE c_std_11)

target_link_libraries(hello-ppu-avc2 PRIVATE
    sysutil_avc2_stub sysmodule_stub c_stub sysutil rt lv2
)

ps3_add_self(hello-ppu-avc2)
