cmake_minimum_required(VERSION 3.20)
project(hello-ppu-ostream-iter-byval CXX)

include(ps3-self)

add_executable(hello-ppu-ostream-iter-byval source/main.cpp)
target_compile_features(hello-ppu-ostream-iter-byval PRIVATE cxx_std_17)
target_link_libraries(hello-ppu-ostream-iter-byval PRIVATE sysutil m)

ps3_add_self(hello-ppu-ostream-iter-byval)
