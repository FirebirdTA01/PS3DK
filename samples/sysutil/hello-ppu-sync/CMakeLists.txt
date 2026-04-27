# hello-ppu-sync — libsync + libsync2 end-to-end validation test.

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-sync C)

include(ps3-self)

add_executable(hello-ppu-sync source/main.c)
target_compile_features(hello-ppu-sync PRIVATE c_std_11)

target_link_libraries(hello-ppu-sync PRIVATE
    sync_stub sync2_stub sysmodule_stub io sysutil rt lv2
)

ps3_add_self(hello-ppu-sync)
