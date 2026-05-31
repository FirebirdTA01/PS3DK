cmake_minimum_required(VERSION 3.20)
project(hello-psgl-light-state C)

include(ps3-self)

add_executable(hello-psgl-light-state source/main.c)
target_compile_features(hello-psgl-light-state PRIVATE c_std_11)
target_link_libraries(hello-psgl-light-state PRIVATE
    PSGL
    PSGLU
    cgb
    fs_stub
    gcm_cmd
    gcm_sys
    rsx
    sysutil_stub
    sysutil
    m)

ps3_add_self(hello-psgl-light-state)
