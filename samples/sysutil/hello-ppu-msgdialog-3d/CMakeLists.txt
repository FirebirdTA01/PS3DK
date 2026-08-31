cmake_minimum_required(VERSION 3.20)
project(hello-ppu-msgdialog-3d C)

include(ps3-self)

add_executable(hello-ppu-msgdialog-3d source/main.c)
target_compile_features(hello-ppu-msgdialog-3d PRIVATE c_std_11)

target_link_libraries(hello-ppu-msgdialog-3d PRIVATE
    sysutil_stub sysutil rt lv2
)

ps3_add_self(hello-ppu-msgdialog-3d)

ps3_add_pkg(hello-ppu-msgdialog-3d
    APPID     "SYUT00015"
    CONTENTID "UP0001-SYUT00015_00-PS3DKSAMPLES0000")
