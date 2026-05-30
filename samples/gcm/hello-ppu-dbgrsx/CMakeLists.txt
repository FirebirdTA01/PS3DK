cmake_minimum_required(VERSION 3.20)
project(hello-ppu-dbgrsx C)

include(ps3-self)

add_executable(hello-ppu-dbgrsx source/main.c)
target_compile_features(hello-ppu-dbgrsx PRIVATE c_std_11)

target_link_libraries(hello-ppu-dbgrsx PRIVATE
    dbgrsx c_stub sysutil rt lv2
)

ps3_add_self(hello-ppu-dbgrsx)
