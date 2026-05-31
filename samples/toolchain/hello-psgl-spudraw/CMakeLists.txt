cmake_minimum_required(VERSION 3.20)
project(hello-psgl-spudraw C ASM)

include(ps3-self)

add_executable(hello-psgl-spudraw source/main.c)
target_compile_features(hello-psgl-spudraw PRIVATE c_std_11)
target_include_directories(hello-psgl-spudraw PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(hello-psgl-spudraw PRIVATE
    PSGL PSGLU cgb fs_stub gcm_cmd gcm_sys rsx sysutil_stub sysutil m c_stub rt lv2)

ps3_add_spu_image(hello-psgl-spudraw
    NAME spu_hole_fill
    SOURCES spu/source/spu_hole_fill.c
    LIBS spuPSGL sputhread)

ps3_add_self(hello-psgl-spudraw)
