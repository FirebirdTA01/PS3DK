cmake_minimum_required(VERSION 3.20)
project(hello-ppu-dtoa-minimal C)

include(ps3-self)

add_executable(hello-ppu-dtoa-minimal source/main.c)
target_compile_features(hello-ppu-dtoa-minimal PRIVATE c_std_11)

target_link_libraries(hello-ppu-dtoa-minimal PRIVATE sysutil m)

ps3_add_self(hello-ppu-dtoa-minimal)
