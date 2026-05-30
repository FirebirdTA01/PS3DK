cmake_minimum_required(VERSION 3.20)
project(hello-ppu-resc C)

include(ps3-self)

add_executable(hello-ppu-resc source/main.c)
target_compile_features(hello-ppu-resc PRIVATE c_std_11)

target_link_libraries(hello-ppu-resc PRIVATE
    resc_stub sysmodule_stub gcm_sys_stub c_stub sysutil rt lv2
)

ps3_add_self(hello-ppu-resc)
