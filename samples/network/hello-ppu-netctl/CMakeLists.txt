cmake_minimum_required(VERSION 3.20)
project(hello-ppu-netctl CXX)

include(ps3-self)

add_executable(hello-ppu-netctl source/main.cpp)
target_compile_features(hello-ppu-netctl PRIVATE cxx_std_17)

target_link_libraries(hello-ppu-netctl PRIVATE
    netctl_stub sysmodule_stub c_stub sysutil rt lv2 m
)

ps3_add_self(hello-ppu-netctl)
