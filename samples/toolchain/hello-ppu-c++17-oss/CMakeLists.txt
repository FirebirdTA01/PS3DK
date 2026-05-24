cmake_minimum_required(VERSION 3.20)
project(hello-ppu-c++17-oss CXX)

include(ps3-self)

add_executable(hello-ppu-c++17-oss source/main.cpp)
target_compile_features(hello-ppu-c++17-oss PRIVATE cxx_std_17)
target_link_libraries(hello-ppu-c++17-oss PRIVATE sysutil m)

ps3_add_self(hello-ppu-c++17-oss)
