cmake_minimum_required(VERSION 3.20)
project(hello-ppu-mlp64-tagged-pointer C)

include(ps3-self)

add_executable(hello-ppu-mlp64-tagged-pointer-ilp32 source/main.c)
target_link_libraries(hello-ppu-mlp64-tagged-pointer-ilp32 PRIVATE sysutil m)
ps3_add_self(hello-ppu-mlp64-tagged-pointer-ilp32)

add_executable(hello-ppu-mlp64-tagged-pointer-lp64 source/main.c)
target_compile_options(hello-ppu-mlp64-tagged-pointer-lp64 PRIVATE -mlp64)
target_link_options(hello-ppu-mlp64-tagged-pointer-lp64 PRIVATE -mlp64)
target_link_libraries(hello-ppu-mlp64-tagged-pointer-lp64 PRIVATE sysutil m)
ps3_add_self(hello-ppu-mlp64-tagged-pointer-lp64)
