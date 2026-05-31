cmake_minimum_required(VERSION 3.20)
project(hello-psgl-glsl-object C ASM)

include(ps3-self)

add_executable(hello-psgl-glsl-object source/main.c)
target_compile_features(hello-psgl-glsl-object PRIVATE c_std_11)
target_link_libraries(hello-psgl-glsl-object PRIVATE
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

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(PSGL_RSX_CG_COMPILER
    NAMES rsx-cg-compiler
    PATHS "$ENV{PS3DEV}/bin" "$ENV{PS3DK}/bin"
    NO_DEFAULT_PATH)
if(NOT PSGL_RSX_CG_COMPILER)
    find_program(PSGL_RSX_CG_COMPILER NAMES rsx-cg-compiler REQUIRED)
endif()

set(_translator "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/psgl/glsl_subset_to_cg.py")
set(_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(_vertex_cg "${_shader_dir}/vshader.vcg")
set(_fragment_cg "${_shader_dir}/fshader.fcg")
set(_vertex_bin "${_shader_dir}/vshader.vpo")
set(_fragment_bin "${_shader_dir}/fshader.fpo")

add_custom_command(
    OUTPUT "${_vertex_cg}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_shader_dir}"
    COMMAND "${Python3_EXECUTABLE}" "${_translator}" vertex
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vshader.glsl"
            "${_vertex_cg}"
    DEPENDS "${_translator}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vshader.glsl"
    COMMENT "Translating vertex GLSL subset"
    VERBATIM)

add_custom_command(
    OUTPUT "${_fragment_cg}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_shader_dir}"
    COMMAND "${Python3_EXECUTABLE}" "${_translator}" fragment
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fshader.glsl"
            "${_fragment_cg}"
    DEPENDS "${_translator}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fshader.glsl"
    COMMENT "Translating fragment GLSL subset"
    VERBATIM)

add_custom_command(
    OUTPUT "${_vertex_bin}"
    COMMAND "${PSGL_RSX_CG_COMPILER}" -p sce_vp_rsx --emit-container
            "${_vertex_bin}" "${_vertex_cg}"
    DEPENDS "${_vertex_cg}"
    COMMENT "Compiling vertex shader container"
    VERBATIM)

add_custom_command(
    OUTPUT "${_fragment_bin}"
    COMMAND "${PSGL_RSX_CG_COMPILER}" -p sce_fp_rsx --emit-container
            "${_fragment_bin}" "${_fragment_cg}"
    DEPENDS "${_fragment_cg}"
    COMMENT "Compiling fragment shader container"
    VERBATIM)

ps3_bin2s(hello-psgl-glsl-object "${_vertex_bin}")
ps3_bin2s(hello-psgl-glsl-object "${_fragment_bin}")
ps3_add_self(hello-psgl-glsl-object)
