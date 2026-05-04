cmake_minimum_required(VERSION 3.20)
project(hello-ppu-usbd CXX)

include(ps3-self)

add_executable(hello-ppu-usbd source/main.cpp)
target_compile_features(hello-ppu-usbd PRIVATE cxx_std_17)

target_link_libraries(hello-ppu-usbd PRIVATE
    usbd_stub sysmodule_stub c_stub sysutil rt lv2 m
)

ps3_add_self(hello-ppu-usbd)
