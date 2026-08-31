cmake_minimum_required(VERSION 3.20)
project(hello-ppu-face C)

include(ps3-self)

add_executable(hello-ppu-face source/main.c)
target_compile_features(hello-ppu-face PRIVATE c_std_11)

target_link_libraries(hello-ppu-face PRIVATE
    face c_stub sysutil rt lv2
)

ps3_add_self(hello-ppu-face)

ps3_add_pkg(hello-ppu-face
    APPID     "SYUT00006"
    CONTENTID "UP0001-SYUT00006_00-PS3DKSAMPLES0000")
