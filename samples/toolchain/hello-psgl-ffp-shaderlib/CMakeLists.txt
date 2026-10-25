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

# Prefer packaged host tool: psgl-shader-builder (.exe on Windows).
find_program(PSGL_SHADER_BUILDER
    NAMES psgl-shader-builder psgl_shader_builder
    PATHS "$ENV{PS3DEV}/bin" "$ENV{PS3DK}/bin"
    NO_DEFAULT_PATH)
if(NOT PSGL_SHADER_BUILDER)
    find_program(PSGL_SHADER_BUILDER NAMES psgl-shader-builder psgl_shader_builder)
endif()

if(PSGL_SHADER_BUILDER)
    set(_builder "${PSGL_SHADER_BUILDER}")
else()
    set(_builder_src "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/psgl-shader-builder/psgl_shader_builder.c")
    set(_emit_src "${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk/libPSGL/src/ffp_emit.c")
    set(_emit_hdr "${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk/libPSGL/src/ffp_emit.h")

    if(EXISTS "${_builder_src}" AND EXISTS "${_emit_src}" AND EXISTS "${_emit_hdr}")
        find_program(PS3_HOST_CC NAMES cc gcc clang)
        if(NOT PS3_HOST_CC)
            message(FATAL_ERROR
                "hello-psgl-ffp-shaderlib: psgl-shader-builder host tool not found, "
                "and no host C compiler (cc/gcc/clang or PS3_HOST_CC) available to build from source.")
        endif()

        set(_ver_file "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/VERSION")
        set(_builder_ver_flag "")
        if(EXISTS "${_ver_file}")
            file(READ "${_ver_file}" _builder_ver)
            string(STRIP "${_builder_ver}" _builder_ver)
            if(_builder_ver)
                set(_builder_ver_flag "-DPSGL_SHADER_BUILDER_VERSION=\"${_builder_ver}\"")
            endif()
        endif()

        set(_host_exe_suffix "")
        if(CMAKE_HOST_WIN32)
            set(_host_exe_suffix ".exe")
        endif()
        set(_builder "${CMAKE_CURRENT_BINARY_DIR}/psgl_shader_builder${_host_exe_suffix}")
        add_custom_command(
            OUTPUT "${_builder}"
            COMMAND "${PS3_HOST_CC}" -std=c11 -O2 ${_builder_ver_flag}
                    "-I${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk/libPSGL/src"
                    "${_builder_src}"
                    "${_emit_src}"
                    -o "${_builder}"
            DEPENDS
                    "${_builder_src}"
                    "${_emit_src}"
                    "${_emit_hdr}"
            COMMENT "Building psgl_shader_builder from source fallback"
            VERBATIM)
    else()
        message(FATAL_ERROR
            "hello-psgl-ffp-shaderlib: psgl-shader-builder host tool not found in "
            "PS3DEV/bin or PS3DK/bin, and builder sources are not present.")
    endif()
endif()

set(_shader_bin "${CMAKE_CURRENT_BINARY_DIR}/shaders.bin")
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
