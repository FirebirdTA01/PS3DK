cmake_minimum_required(VERSION 3.20)
project(hello-ppu-avc C)

include(ps3-self)

add_executable(hello-ppu-avc source/main.c)
target_compile_features(hello-ppu-avc PRIVATE c_std_11)

target_link_libraries(hello-ppu-avc PRIVATE
    sysutil_stub sysutil rt lv2
)

ps3_add_self(hello-ppu-avc)
