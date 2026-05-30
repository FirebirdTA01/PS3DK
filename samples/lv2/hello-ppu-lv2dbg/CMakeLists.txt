cmake_minimum_required(VERSION 3.20)
project(hello-ppu-lv2dbg C)

include(ps3-self)

add_executable(hello-ppu-lv2dbg source/main.c)
target_compile_features(hello-ppu-lv2dbg PRIVATE c_std_11)

target_link_libraries(hello-ppu-lv2dbg PRIVATE
    lv2dbg_stub sysmodule_stub c_stub sysutil rt lv2
)

ps3_add_self(hello-ppu-lv2dbg)
