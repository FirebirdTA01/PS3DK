cmake_minimum_required(VERSION 3.20)
project(hello-ppu-pthread C)

include(ps3-self)

add_executable(hello-ppu-pthread source/main.c)
target_compile_features(hello-ppu-pthread PRIVATE c_std_11)
set_property(TARGET hello-ppu-pthread PROPERTY C_EXTENSIONS ON)

ps3_add_self(hello-ppu-pthread)
