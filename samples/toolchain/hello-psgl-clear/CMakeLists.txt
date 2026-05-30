cmake_minimum_required(VERSION 3.20)
project(hello-psgl-clear C)

include(ps3-self)

add_executable(hello-psgl-clear source/main.c)
target_link_libraries(hello-psgl-clear PRIVATE PSGL PSGLU gcm_cmd gcm_sys rsx sysutil_stub sysutil m)

ps3_add_self(hello-psgl-clear)
