cmake_minimum_required(VERSION 3.20)
project(hello-psgl-blend C ASM)

include(ps3-self)

add_executable(hello-psgl-blend source/main.c)
target_compile_features(hello-psgl-blend PRIVATE c_std_11)
target_link_libraries(hello-psgl-blend PRIVATE
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

ps3_add_cg_shader_rsxcgc(hello-psgl-blend shaders/vpshader.vcg)
ps3_add_cg_shader_rsxcgc(hello-psgl-blend shaders/fp_red.fcg)
ps3_add_cg_shader_rsxcgc(hello-psgl-blend shaders/fp_blue_alpha.fcg)
ps3_add_self(hello-psgl-blend)
