cmake_minimum_required(VERSION 3.20)
project(hello-psgl-cg-passthrough C)

include(ps3-self)

add_executable(hello-psgl-cg-passthrough source/main.c)
target_link_libraries(hello-psgl-cg-passthrough PRIVATE PSGL PSGLU cgb fs_stub gcm_cmd gcm_sys rsx sysutil_stub sysutil m)

ps3_add_self(hello-psgl-cg-passthrough)
