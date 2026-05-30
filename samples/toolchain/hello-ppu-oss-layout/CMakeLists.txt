cmake_minimum_required(VERSION 3.20)
project(hello-ppu-oss-layout CXX)

include(ps3-self)

add_executable(hello-ppu-oss-layout source/main.cpp)
target_compile_features(hello-ppu-oss-layout PRIVATE cxx_std_17)
target_link_libraries(hello-ppu-oss-layout PRIVATE sysutil m)

ps3_add_self(hello-ppu-oss-layout)
