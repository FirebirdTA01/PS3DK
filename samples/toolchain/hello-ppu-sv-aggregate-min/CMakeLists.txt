cmake_minimum_required(VERSION 3.20)
project(hello-ppu-sv-aggregate-min CXX)

include(ps3-self)

add_executable(hello-ppu-sv-aggregate-min source/main.cpp)
target_compile_features(hello-ppu-sv-aggregate-min PRIVATE cxx_std_17)
target_link_libraries(hello-ppu-sv-aggregate-min PRIVATE sysutil m)

ps3_add_self(hello-ppu-sv-aggregate-min)
