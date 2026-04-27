cmake_minimum_required(VERSION 3.20)
project(hello-ppu-keyboard C)

include(ps3-self)

add_executable(hello-ppu-keyboard source/main.c)
target_compile_features(hello-ppu-keyboard PRIVATE c_std_11)

target_link_libraries(hello-ppu-keyboard PRIVATE
    io sysutil rt lv2
)

ps3_add_self(hello-ppu-keyboard)
