cmake_minimum_required(VERSION 3.20)

project(sprx_vsh_plugin_regression C)

# The VSH plugin sample's loader path (card t_2023e869): a game process loads,
# starts, stops and unloads the plugin twice; module_start sees a caller and
# returns, the worker thread made with sys_ppu_thread_create ticks a .data
# and a .bss counter and writes its log with fsync, and module_stop joins it
# and returns.  The loader prints a verdict per step and VSHP_ALL_PASS.
# The plugin loader's path (no caller, no TLS) runs only on a console.
#
# Built from the shipped sample (add_subdirectory), not a copy, for both ABIs.
# RPCS3 mounts /app_home to the booted file's directory, so each boot file is
# staged in a directory holding only the plugin container it must load
# (issue #20):
#
#   ./      <row>.fake.self   + <stem>.fake.sprx   rows sprx-vsh-plugin[-lp64]
#   raw/    <row>.elf         + <stem>.prx         rows sprx-vsh-plugin-raw[-lp64]
#   real/   <row>.self        + <stem>.sprx        rows sprx-vsh-plugin-real[-lp64]

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    message(STATUS "sprx-vsh-plugin: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

set(_sample "${CMAKE_CURRENT_SOURCE_DIR}/../../../samples/sprx/vsh-plugin")
if(NOT EXISTS "${_sample}/CMakeLists.txt")
    message(FATAL_ERROR
        "sprx-vsh-plugin: expected the sample at ${_sample}.\n"
        "  This probe builds the sample rather than copying it; if it "
        "moved, fix the path here rather than duplicating the sources.")
endif()

add_subdirectory("${_sample}" "${CMAKE_CURRENT_BINARY_DIR}/vsh-plugin")

set(_out "${CMAKE_CURRENT_BINARY_DIR}/vsh-plugin")
set(_dst "${CMAKE_CURRENT_SOURCE_DIR}")
set(_stage)
foreach(_sfx "" "-lp64")
    string(REPLACE "-" "_" _msfx "${_sfx}")
    set(_stem "vsh_plugin${_msfx}")
    set(_ldr "vsh-plugin-loader${_sfx}")
    set(_row "sprx-vsh-plugin${_sfx}")
    list(APPEND _stage
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_out}/${_stem}.fake.sprx" "${_dst}/${_stem}.fake.sprx"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_out}/${_ldr}.fake.self" "${_dst}/${_row}.fake.self"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_out}/${_stem}.prx" "${_dst}/raw/${_stem}.prx"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_out}/${_ldr}.elf" "${_dst}/raw/${_row}.elf"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_out}/${_stem}.sprx" "${_dst}/real/${_stem}.sprx"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_out}/${_ldr}.self" "${_dst}/real/${_row}.self")
endforeach()
add_custom_target(sprx-vsh-plugin ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dst}/raw" "${_dst}/real"
    ${_stage}
    DEPENDS vsh-plugin-module vsh-plugin-loader
            vsh-plugin-module-lp64 vsh-plugin-loader-lp64
    COMMENT "sprx-vsh-plugin: staging each container beside its loader"
    VERBATIM)
