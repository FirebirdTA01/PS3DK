cmake_minimum_required(VERSION 3.20)
project(hello-ppu-oss-manual-recipe CXX)

include(ps3-self)

add_executable(hello-ppu-oss-manual-recipe source/main.cpp)
target_compile_features(hello-ppu-oss-manual-recipe PRIVATE cxx_std_17)
target_link_libraries(hello-ppu-oss-manual-recipe PRIVATE sysutil m)

ps3_add_self(hello-ppu-oss-manual-recipe)
