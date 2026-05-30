cmake_minimum_required(VERSION 3.20)
project(hello-ppu-np-clans CXX)

include(ps3-self)

add_executable(hello-ppu-np-clans source/main.cpp)
target_compile_features(hello-ppu-np-clans PRIVATE cxx_std_17)

target_link_libraries(hello-ppu-np-clans PRIVATE
    sysutil_np_clans_stub sysmodule_stub c_stub sysutil rt lv2 m
)

ps3_add_self(hello-ppu-np-clans)
