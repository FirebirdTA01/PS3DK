cmake_minimum_required(VERSION 3.20)
project(hello-psgl-shader-interface C ASM)

include(ps3-self)

add_executable(hello-psgl-shader-interface source/main.c)
target_compile_features(hello-psgl-shader-interface PRIVATE c_std_11)
target_link_libraries(hello-psgl-shader-interface PRIVATE
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

ps3_add_cg_shader_rsxcgc(hello-psgl-shader-interface shaders/vpshader.vcg)
ps3_add_cg_shader_rsxcgc(hello-psgl-shader-interface shaders/fpshader.fcg)
ps3_add_self(hello-psgl-shader-interface)
