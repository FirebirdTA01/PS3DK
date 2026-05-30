cmake_minimum_required(VERSION 3.20)
project(hello-ppu-ovis C)

include(ps3-self)

add_executable(hello-ppu-ovis source/main.c)
target_compile_features(hello-ppu-ovis PRIVATE c_std_11)

target_link_libraries(hello-ppu-ovis PRIVATE
    ovis_stub c_stub sysutil rt lv2
)

ps3_add_self(hello-ppu-ovis)
