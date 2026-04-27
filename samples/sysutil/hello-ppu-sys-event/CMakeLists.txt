cmake_minimum_required(VERSION 3.20)
project(hello-ppu-sys-event C)

include(ps3-self)

add_executable(hello-ppu-sys-event source/main.c)
target_compile_features(hello-ppu-sys-event PRIVATE c_std_11)

target_link_libraries(hello-ppu-sys-event PRIVATE
    sysutil rt lv2
)

ps3_add_self(hello-ppu-sys-event)
