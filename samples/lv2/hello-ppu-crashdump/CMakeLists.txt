cmake_minimum_required(VERSION 3.20)
project(hello-ppu-crashdump CXX)

include(ps3-self)

add_executable(hello-ppu-crashdump source/main.cpp)
target_compile_features(hello-ppu-crashdump PRIVATE cxx_std_17)

target_link_libraries(hello-ppu-crashdump PRIVATE
    sys_crashdump_stub c_stub rt lv2 m
)

ps3_add_self(hello-ppu-crashdump)
