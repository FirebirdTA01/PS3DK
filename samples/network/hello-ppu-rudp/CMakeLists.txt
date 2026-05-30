cmake_minimum_required(VERSION 3.20)
project(hello-ppu-rudp C)

include(ps3-self)

add_executable(hello-ppu-rudp source/main.c)
target_compile_features(hello-ppu-rudp PRIVATE c_std_11)

target_link_libraries(hello-ppu-rudp PRIVATE
    rudp_stub sysmodule_stub net_stub c_stub sysutil rt lv2
)

ps3_add_self(hello-ppu-rudp)
