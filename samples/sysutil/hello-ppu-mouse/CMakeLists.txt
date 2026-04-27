cmake_minimum_required(VERSION 3.20)
project(hello-ppu-mouse C)

include(ps3-self)

add_executable(hello-ppu-mouse source/main.c)
target_compile_features(hello-ppu-mouse PRIVATE c_std_11)

target_link_libraries(hello-ppu-mouse PRIVATE
    io sysutil rt lv2
)

ps3_add_self(hello-ppu-mouse)
