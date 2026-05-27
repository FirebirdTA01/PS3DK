# hello-ppu-gamecontent - CMake build.

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-gamecontent C)

include(ps3-self)

add_executable(hello-ppu-gamecontent source/main.c)
target_compile_features(hello-ppu-gamecontent PRIVATE c_std_11)

target_link_libraries(hello-ppu-gamecontent PRIVATE
    cellGame_stub sysutil_stub sysmodule_stub io sysutil rt lv2
)

ps3_add_self(hello-ppu-gamecontent)

ps3_add_pkg(hello-ppu-gamecontent
    TITLE     "PS3DK GameContent Validation"
    APPID     "GAMECT001"
    CONTENTID "UP0001-GAMECT001_00-PS3DKTOOLCHAIN00")
