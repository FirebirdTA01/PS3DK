cmake_minimum_required(VERSION 3.20)
project(hello-ppu-sysconf C)

include(ps3-self)

add_executable(hello-ppu-sysconf source/main.c)
target_compile_features(hello-ppu-sysconf PRIVATE c_std_11)

target_link_libraries(hello-ppu-sysconf PRIVATE
    sysutil_stub sysutil rt lv2
)

ps3_add_self(hello-ppu-sysconf)

ps3_add_pkg(hello-ppu-sysconf
    APPID     "SYUT00035"
    CONTENTID "UP0001-SYUT00035_00-PS3DKSAMPLES0000")
