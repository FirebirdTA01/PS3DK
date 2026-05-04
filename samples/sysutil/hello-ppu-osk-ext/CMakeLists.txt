cmake_minimum_required(VERSION 3.20)
project(hello-ppu-osk-ext CXX)

include(ps3-self)

add_executable(hello-ppu-osk-ext source/main.cpp)
target_compile_features(hello-ppu-osk-ext PRIVATE cxx_std_17)

target_link_libraries(hello-ppu-osk-ext PRIVATE
    sysutil_oskdialog_ext_stub sysmodule_stub c_stub sysutil rt lv2 m
)

ps3_add_self(hello-ppu-osk-ext)
