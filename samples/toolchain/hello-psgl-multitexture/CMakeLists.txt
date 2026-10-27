cmake_minimum_required(VERSION 3.20)
project(hello-psgl-multitexture C ASM)

include(ps3-self)

add_executable(hello-psgl-multitexture source/main.c)
target_compile_features(hello-psgl-multitexture PRIVATE c_std_11)
target_link_libraries(hello-psgl-multitexture PRIVATE
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
        "hello-psgl-multitexture: ${PSGL_RSX_CG_COMPILER} does not support "
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
add_custom_target(hello-psgl-multitexture-bin DEPENDS "${_shader_bin}")
add_dependencies(hello-psgl-multitexture hello-psgl-multitexture-bin)
ps3_bin2s(hello-psgl-multitexture "${_shader_bin}")

ps3_add_self(hello-psgl-multitexture)
