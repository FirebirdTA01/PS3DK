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
# Each .fake.self is staged here with its .fake.sprx, because RPCS3 mounts
# /app_home to the booted file's directory.

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
add_custom_target(sprx-vsh-plugin ALL
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_out}/vsh_plugin.fake.sprx" "${CMAKE_CURRENT_SOURCE_DIR}/vsh_plugin.fake.sprx"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_out}/vsh-plugin-loader.fake.self" "${CMAKE_CURRENT_SOURCE_DIR}/sprx-vsh-plugin.fake.self"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_out}/vsh_plugin_lp64.fake.sprx" "${CMAKE_CURRENT_SOURCE_DIR}/vsh_plugin_lp64.fake.sprx"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_out}/vsh-plugin-loader-lp64.fake.self" "${CMAKE_CURRENT_SOURCE_DIR}/sprx-vsh-plugin-lp64.fake.self"
    DEPENDS vsh-plugin-module vsh-plugin-loader
            vsh-plugin-module-lp64 vsh-plugin-loader-lp64
    COMMENT "sprx-vsh-plugin: staging the plugins beside their loaders"
    VERBATIM)
