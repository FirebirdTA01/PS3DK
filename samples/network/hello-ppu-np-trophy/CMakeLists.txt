cmake_minimum_required(VERSION 3.20)
project(hello-ppu-np-trophy CXX)

include(ps3-self)

add_executable(hello-ppu-np-trophy source/main.cpp)
target_compile_features(hello-ppu-np-trophy PRIVATE cxx_std_17)

target_link_libraries(hello-ppu-np-trophy PRIVATE
    sysutil_np_trophy_stub sysmodule_stub c_stub sysutil rt lv2 m
)

ps3_add_self(hello-ppu-np-trophy)
