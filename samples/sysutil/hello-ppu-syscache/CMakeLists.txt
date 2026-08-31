cmake_minimum_required(VERSION 3.20)
project(hello-ppu-syscache C)

include(ps3-self)

add_executable(hello-ppu-syscache source/main.c)
target_compile_features(hello-ppu-syscache PRIVATE c_std_11)

target_link_libraries(hello-ppu-syscache PRIVATE
    sysutil_stub sysutil rt lv2
)

ps3_add_self(hello-ppu-syscache)

ps3_add_pkg(hello-ppu-syscache
    APPID     "SYUT00034"
    CONTENTID "UP0001-SYUT00034_00-PS3DKSAMPLES0000")
