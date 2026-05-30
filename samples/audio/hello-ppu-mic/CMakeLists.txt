cmake_minimum_required(VERSION 3.20)
project(hello-ppu-mic C)

include(ps3-self)

add_executable(hello-ppu-mic source/main.c)
target_compile_features(hello-ppu-mic PRIVATE c_std_11)

target_link_libraries(hello-ppu-mic PRIVATE
    mic_stub sysmodule_stub c_stub sysutil rt lv2
)

ps3_add_self(hello-ppu-mic)
