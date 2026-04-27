cmake_minimum_required(VERSION 3.20)
project(hello-ppu-osk C)

include(ps3-self)

add_executable(hello-ppu-osk source/main.c)
target_compile_features(hello-ppu-osk PRIVATE c_std_11)

target_link_libraries(hello-ppu-osk PRIVATE
    sysutil_stub sysutil rt lv2
)

ps3_add_self(hello-ppu-osk)
