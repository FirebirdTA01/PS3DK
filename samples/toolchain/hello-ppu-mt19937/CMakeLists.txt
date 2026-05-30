cmake_minimum_required(VERSION 3.20)
project(hello-ppu-mt19937 C)

include(ps3-self)

add_executable(hello-ppu-mt19937 source/main.c)
target_compile_features(hello-ppu-mt19937 PRIVATE c_std_11)

target_link_libraries(hello-ppu-mt19937 PRIVATE
    mt19937 c_stub sysutil rt lv2
)

ps3_add_self(hello-ppu-mt19937)
