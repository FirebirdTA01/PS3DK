cmake_minimum_required(VERSION 3.20)
project(hello-ppu-lp64-syscall-probe C)

include(ps3-self)

add_executable(hello-ppu-lp64-syscall-probe main.c)
target_compile_options(hello-ppu-lp64-syscall-probe PRIVATE -mlp64)
target_link_options(hello-ppu-lp64-syscall-probe PRIVATE -mlp64)
target_link_libraries(hello-ppu-lp64-syscall-probe PRIVATE rsx gcm_cmd gcm_sys sysutil_stub sysutil m)
ps3_add_self(hello-ppu-lp64-syscall-probe)
