cmake_minimum_required(VERSION 3.20)
project(hello-psgl-spudraw-triangle C ASM)

include(ps3-self)

add_executable(hello-psgl-spudraw-triangle source/main.c)
target_compile_features(hello-psgl-spudraw-triangle PRIVATE c_std_11)
target_include_directories(hello-psgl-spudraw-triangle PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(hello-psgl-spudraw-triangle PRIVATE
    PSGL PSGLU cgb fs_stub gcm_cmd gcm_sys rsx sysutil_stub sysutil m c_stub rt lv2)

ps3_add_cg_shader_rsxcgc(hello-psgl-spudraw-triangle shaders/vpshader.vcg)
ps3_add_cg_shader_rsxcgc(hello-psgl-spudraw-triangle shaders/fpshader.fcg)

ps3_add_spu_image(hello-psgl-spudraw-triangle
    NAME spu_hole_fill
    SOURCES spu/source/spu_hole_fill.c
    LIBS spuPSGL sputhread)

ps3_add_self(hello-psgl-spudraw-triangle)
