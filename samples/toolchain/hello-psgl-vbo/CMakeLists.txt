cmake_minimum_required(VERSION 3.20)
project(hello-psgl-vbo C)

include(ps3-self)

add_executable(hello-psgl-vbo source/main.c)
target_link_libraries(hello-psgl-vbo PRIVATE PSGL PSGLU gcm_cmd gcm_sys rsx sysutil_stub sysutil m)

ps3_add_self(hello-psgl-vbo)
