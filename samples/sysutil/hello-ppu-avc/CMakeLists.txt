cmake_minimum_required(VERSION 3.20)
project(hello-ppu-avc C)

include(ps3-self)

add_executable(hello-ppu-avc source/main.c)
target_compile_features(hello-ppu-avc PRIVATE c_std_11)

target_link_libraries(hello-ppu-avc PRIVATE
    sysutil_stub sysutil rt lv2
)

ps3_add_self(hello-ppu-avc)

ps3_add_pkg(hello-ppu-avc
    APPID     "SYUT00002"
    CONTENTID "UP0001-SYUT00002_00-PS3DKSAMPLES0000")
