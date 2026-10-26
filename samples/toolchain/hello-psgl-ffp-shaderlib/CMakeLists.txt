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

find_program(PSGL_RSX_CG_COMPILER
    NAMES rsx-cg-compiler
    PATHS "$ENV{PS3DEV}/bin" "$ENV{PS3DK}/bin"
    NO_DEFAULT_PATH)
if(NOT PSGL_RSX_CG_COMPILER)
    find_program(PSGL_RSX_CG_COMPILER NAMES rsx-cg-compiler REQUIRED)
endif()

# rsx-cg-compiler builds the library itself with --psgl-ffp-library, which
# it lists in --help.  The SDK ships a matching compiler, so one without the
# mode is a misconfiguration and is refused here.
execute_process(
    COMMAND "${PSGL_RSX_CG_COMPILER}" --help
    OUTPUT_VARIABLE _rsxcg_help_out
    ERROR_VARIABLE _rsxcg_help_err
    RESULT_VARIABLE _rsxcg_help_rc)
set(_rsxcg_ffp_pos -1)
if(_rsxcg_help_rc EQUAL 0)
    string(FIND "${_rsxcg_help_out}${_rsxcg_help_err}" "--psgl-ffp-library" _rsxcg_ffp_pos)
endif()
if(_rsxcg_ffp_pos EQUAL -1)
    message(FATAL_ERROR
        "hello-psgl-ffp-shaderlib: ${PSGL_RSX_CG_COMPILER} does not support "
        "--psgl-ffp-library (its --help exited ${_rsxcg_help_rc}; a supporting compiler exits 0 and lists the mode). "
        "An rsx-cg-compiler with --psgl-ffp-library is required: use the "
        "rsx-cg-compiler from the same SDK as this sample, or set "
        "PSGL_RSX_CG_COMPILER to it.")
endif()

set(_shader_bin "${CMAKE_CURRENT_BINARY_DIR}/shaders.bin")
add_custom_command(
    OUTPUT "${_shader_bin}"
    COMMAND "${PSGL_RSX_CG_COMPILER}" --psgl-ffp-library
            "${CMAKE_CURRENT_SOURCE_DIR}/state.txt"
            -o "${_shader_bin}"
    DEPENDS "${PSGL_RSX_CG_COMPILER}" "${CMAKE_CURRENT_SOURCE_DIR}/state.txt"
    COMMENT "Generating fixed-function shader library"
    VERBATIM)
add_custom_target(hello-psgl-ffp-shaderlib-bin DEPENDS "${_shader_bin}")
add_dependencies(hello-psgl-ffp-shaderlib hello-psgl-ffp-shaderlib-bin)
ps3_bin2s(hello-psgl-ffp-shaderlib "${_shader_bin}")

ps3_add_self(hello-psgl-ffp-shaderlib)
