cmake_minimum_required(VERSION 3.20)
project(hello-ppu-syscache C)

include(ps3-self)

add_executable(hello-ppu-syscache source/main.c)
target_compile_features(hello-ppu-syscache PRIVATE c_std_11)

target_link_libraries(hello-ppu-syscache PRIVATE
    sysutil_stub sysutil rt lv2
)

ps3_add_self(hello-ppu-syscache)
