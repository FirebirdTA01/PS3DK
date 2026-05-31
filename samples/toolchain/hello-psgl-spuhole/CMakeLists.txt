cmake_minimum_required(VERSION 3.20)
project(hello-psgl-spuhole C)

include(ps3-self)

add_executable(hello-psgl-spuhole source/main.c)
target_compile_features(hello-psgl-spuhole PRIVATE c_std_11)
target_link_libraries(hello-psgl-spuhole PRIVATE
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

ps3_add_self(hello-psgl-spuhole)
