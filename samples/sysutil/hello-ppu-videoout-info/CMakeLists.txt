# hello-ppu-videoout-info — CMake build.
cmake_minimum_required(VERSION 3.20)
project(hello-ppu-videoout-info C)

include(ps3-self)

add_executable(hello-ppu-videoout-info source/main.c)
target_compile_features(hello-ppu-videoout-info PRIVATE c_std_11)

target_link_libraries(hello-ppu-videoout-info PRIVATE
    sysutil_stub sysutil rt lv2
)

ps3_add_self(hello-ppu-videoout-info)
