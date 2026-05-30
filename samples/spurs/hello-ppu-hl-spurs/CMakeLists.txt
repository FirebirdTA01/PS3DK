cmake_minimum_required(VERSION 3.20)
project(hello-ppu-hl-spurs CXX)

include(ps3-self)

add_executable(hello-ppu-hl-spurs source/main.cpp)
target_compile_features(hello-ppu-hl-spurs PRIVATE cxx_std_17)

target_link_libraries(hello-ppu-hl-spurs PRIVATE
    spurs_stub c_stub sysmodule_stub sysutil rt lv2 m
)

ps3_add_self(hello-ppu-hl-spurs)
