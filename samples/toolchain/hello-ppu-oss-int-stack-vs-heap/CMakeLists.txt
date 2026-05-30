cmake_minimum_required(VERSION 3.20)
project(hello-ppu-oss-int-stack-vs-heap CXX)

include(ps3-self)

add_executable(hello-ppu-oss-int-stack-vs-heap source/main.cpp)
target_compile_features(hello-ppu-oss-int-stack-vs-heap PRIVATE cxx_std_17)
target_link_libraries(hello-ppu-oss-int-stack-vs-heap PRIVATE sysutil m)

ps3_add_self(hello-ppu-oss-int-stack-vs-heap)
