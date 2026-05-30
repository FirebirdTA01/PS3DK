cmake_minimum_required(VERSION 3.20)
project(hello-ppu-sheap C)

include(ps3-self)

add_executable(hello-ppu-sheap source/main.c)
target_compile_features(hello-ppu-sheap PRIVATE c_std_11)

target_link_libraries(hello-ppu-sheap PRIVATE
    sheap_stub sysmodule_stub sync_stub c_stub sysutil rt lv2
)

ps3_add_self(hello-ppu-sheap)
