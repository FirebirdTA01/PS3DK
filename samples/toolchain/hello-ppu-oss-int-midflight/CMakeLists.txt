cmake_minimum_required(VERSION 3.20)
project(hello-ppu-oss-int-midflight CXX)

include(ps3-self)

add_executable(hello-ppu-oss-int-midflight source/main.cpp)
target_compile_features(hello-ppu-oss-int-midflight PRIVATE cxx_std_17)
target_link_libraries(hello-ppu-oss-int-midflight PRIVATE sysutil m)

ps3_add_self(hello-ppu-oss-int-midflight)
