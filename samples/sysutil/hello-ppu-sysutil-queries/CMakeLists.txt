cmake_minimum_required(VERSION 3.20)
project(hello-ppu-sysutil-queries C)
include(ps3-self)
add_executable(hello-ppu-sysutil-queries source/main.c)
target_compile_features(hello-ppu-sysutil-queries PRIVATE c_std_11)
target_compile_options(hello-ppu-sysutil-queries PRIVATE -Wall -Wextra)
target_link_libraries(hello-ppu-sysutil-queries PRIVATE
    sysutil_licensearea_stub sysutil_remoteplay_stub sysutil_game_exec_stub
    sysutil_sysconf_ext_stub sysmodule_stub sysutil rt lv2)
ps3_add_self(hello-ppu-sysutil-queries)
