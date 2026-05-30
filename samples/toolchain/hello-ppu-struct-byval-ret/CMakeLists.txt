cmake_minimum_required(VERSION 3.20)
project(hello-ppu-struct-byval-ret CXX)

include(ps3-self)

add_executable(hello-ppu-struct-byval-ret source/main.cpp)
target_compile_features(hello-ppu-struct-byval-ret PRIVATE cxx_std_17)
target_link_libraries(hello-ppu-struct-byval-ret PRIVATE sysutil m)

ps3_add_self(hello-ppu-struct-byval-ret)
