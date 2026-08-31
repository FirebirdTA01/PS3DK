cmake_minimum_required(VERSION 3.20)
project(hello-ppu-font C)
include(ps3-self)
add_executable(hello-ppu-font source/main.c)
target_compile_features(hello-ppu-font PRIVATE c_std_11)
target_link_libraries(hello-ppu-font PRIVATE cellFont_stub rt lv2)
ps3_add_self(hello-ppu-font)

ps3_add_pkg(hello-ppu-font
    APPID     "SYUT00007"
    CONTENTID "UP0001-SYUT00007_00-PS3DKSAMPLES0000")
