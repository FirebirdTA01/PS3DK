cmake_minimum_required(VERSION 3.20)
project(hello-psgl-cmdbuf C)

include(ps3-self)

add_executable(hello-psgl-cmdbuf source/main.c)
target_compile_features(hello-psgl-cmdbuf PRIVATE c_std_11)
target_link_libraries(hello-psgl-cmdbuf PRIVATE
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

ps3_add_self(hello-psgl-cmdbuf)
