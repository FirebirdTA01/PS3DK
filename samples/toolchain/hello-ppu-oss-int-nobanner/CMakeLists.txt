cmake_minimum_required(VERSION 3.20)
project(hello-ppu-oss-int-nobanner CXX)

include(ps3-self)

add_executable(hello-ppu-oss-int-nobanner source/main.cpp)
target_compile_features(hello-ppu-oss-int-nobanner PRIVATE cxx_std_17)
target_link_libraries(hello-ppu-oss-int-nobanner PRIVATE sysutil m)

ps3_add_self(hello-ppu-oss-int-nobanner)
