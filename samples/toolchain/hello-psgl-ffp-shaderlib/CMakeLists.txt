cmake_minimum_required(VERSION 3.20)
project(hello-psgl-ffp-shaderlib C ASM)

include(ps3-self)

add_executable(hello-psgl-ffp-shaderlib source/main.c)
target_compile_features(hello-psgl-ffp-shaderlib PRIVATE c_std_11)
target_link_libraries(hello-psgl-ffp-shaderlib PRIVATE
    PSGL
    PSGLU
    cgb
    pngdec_stub
    sysmodule_stub
    fs_stub
    gcm_cmd
    gcm_sys
    rsx
    sysutil_stub
    sysutil
    m)

set(_icon0_src "${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk/assets/ICON0.PNG")
set(_icon0_dst "${CMAKE_CURRENT_BINARY_DIR}/ICON0.PNG")
configure_file("${_icon0_src}" "${_icon0_dst}" COPYONLY)
ps3_bin2s(hello-psgl-ffp-shaderlib "${_icon0_dst}")

find_program(PS3_HOST_CC NAMES cc gcc clang REQUIRED)
find_program(PSGL_RSX_CG_COMPILER
    NAMES rsx-cg-compiler
    PATHS "$ENV{PS3DEV}/bin" "$ENV{PS3DK}/bin"
    NO_DEFAULT_PATH)
if(NOT PSGL_RSX_CG_COMPILER)
    find_program(PSGL_RSX_CG_COMPILER NAMES rsx-cg-compiler REQUIRED)
endif()

set(_builder "${CMAKE_CURRENT_BINARY_DIR}/psgl_shader_builder")
set(_shader_bin "${CMAKE_CURRENT_BINARY_DIR}/shaders.bin")
add_custom_command(
    OUTPUT "${_builder}"
    COMMAND "${PS3_HOST_CC}" -std=c11 -O2
            "-I${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk/libPSGL/src"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/psgl-shader-builder/psgl_shader_builder.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk/libPSGL/src/ffp_emit.c"
            -o "${_builder}"
    DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/psgl-shader-builder/psgl_shader_builder.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk/libPSGL/src/ffp_emit.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk/libPSGL/src/ffp_emit.h"
    COMMENT "Building psgl_shader_builder"
    VERBATIM)
add_custom_command(
    OUTPUT "${_shader_bin}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaderlib-work"
    COMMAND "${_builder}" "${PSGL_RSX_CG_COMPILER}"
            "${CMAKE_CURRENT_SOURCE_DIR}/state.txt"
            "${_shader_bin}"
            "${CMAKE_CURRENT_BINARY_DIR}/shaderlib-work"
    DEPENDS "${_builder}" "${CMAKE_CURRENT_SOURCE_DIR}/state.txt"
    COMMENT "Generating fixed-function shader library"
    VERBATIM)
add_custom_target(hello-psgl-ffp-shaderlib-bin DEPENDS "${_shader_bin}")
add_dependencies(hello-psgl-ffp-shaderlib hello-psgl-ffp-shaderlib-bin)
ps3_bin2s(hello-psgl-ffp-shaderlib "${_shader_bin}")

ps3_add_self(hello-psgl-ffp-shaderlib)
