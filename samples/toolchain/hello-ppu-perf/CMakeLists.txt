cmake_minimum_required(VERSION 3.20)
project(hello-ppu-perf C)

include(ps3-self)

add_executable(hello-ppu-perf source/main.c)
target_compile_features(hello-ppu-perf PRIVATE c_std_11)

target_link_libraries(hello-ppu-perf PRIVATE
    perf c_stub sysutil rt lv2
)

ps3_add_self(hello-ppu-perf)
