cmake_minimum_required(VERSION 3.20)
project(hello-ppu-tls C)

include(ps3-self)

add_executable(hello-ppu-tls source/main.c)
target_compile_features(hello-ppu-tls PRIVATE c_std_11)

ps3_add_self(hello-ppu-tls)
