cmake_minimum_required(VERSION 3.20)
project(hello-ppu-dmux CXX)

include(ps3-self)

add_executable(hello-ppu-dmux source/main.cpp)
target_compile_features(hello-ppu-dmux PRIVATE cxx_std_17)

target_link_libraries(hello-ppu-dmux PRIVATE
    dmux_stub sysmodule_stub c_stub sysutil rt lv2 m
)

ps3_add_self(hello-ppu-dmux)
