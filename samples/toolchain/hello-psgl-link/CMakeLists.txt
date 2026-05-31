cmake_minimum_required(VERSION 3.20)
project(hello-psgl-link CXX)

include(ps3-self)

add_executable(hello-psgl-link source/main.cpp)
target_compile_features(hello-psgl-link PRIVATE cxx_std_17)
target_link_libraries(hello-psgl-link PRIVATE
    PSGL
    PSGLU
    cgb
    sscgpu
    fs_stub
    gcm_cmd
    gcm_sys
    rsx
    sysutil_stub
    sysutil
    m)

ps3_add_self(hello-psgl-link)
