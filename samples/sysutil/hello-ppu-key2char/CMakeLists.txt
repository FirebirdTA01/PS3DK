cmake_minimum_required(VERSION 3.20)
project(hello-ppu-key2char C)
include(ps3-self)
add_executable(hello-ppu-key2char source/main.c)
target_compile_features(hello-ppu-key2char PRIVATE c_std_11)
target_link_libraries(hello-ppu-key2char PRIVATE
    cellKey2char_stub sysmodule_stub io sysutil rt lv2)
ps3_add_self(hello-ppu-key2char)
