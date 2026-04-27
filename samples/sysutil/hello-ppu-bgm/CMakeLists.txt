cmake_minimum_required(VERSION 3.20)
project(hello-ppu-bgm C)

include(ps3-self)

add_executable(hello-ppu-bgm source/main.c)
target_compile_features(hello-ppu-bgm PRIVATE c_std_11)

target_link_libraries(hello-ppu-bgm PRIVATE
    sysutil_stub sysutil rt lv2
)

ps3_add_self(hello-ppu-bgm)
