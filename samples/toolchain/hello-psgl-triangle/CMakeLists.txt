cmake_minimum_required(VERSION 3.20)
project(hello-psgl-triangle C ASM)

include(ps3-self)

add_executable(hello-psgl-triangle source/main.c)
target_compile_features(hello-psgl-triangle PRIVATE c_std_11)
target_link_libraries(hello-psgl-triangle PRIVATE
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

ps3_add_cg_shader_rsxcgc(hello-psgl-triangle shaders/vpshader.vcg)
ps3_add_cg_shader_rsxcgc(hello-psgl-triangle shaders/fpshader.fcg)
ps3_add_self(hello-psgl-triangle)
