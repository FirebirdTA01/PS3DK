cmake_minimum_required(VERSION 3.20)

project(sprx_segments_regression C ASM)

# Guards the module layout retail Lv-2 needs (issue #137): a code segment and
# a separate data segment, both SPU-readable, and 0x1c export records. The
# module exports two libraries beside the crt's management record, and its
# module_start / module_stop write and read back their own .data and .bss.
# The loader prints a verdict per step and SPRX_SEG_ALL_PASS at the end.
#
# The emulator accepts the old single-segment layout as well, so this row
# alone proves only that the new layout still loads and runs; the layout
# itself is gated by `prx-gen check` in the build. Hardware is the real test.
#
# Built from the shipped sample (add_subdirectory), not a copy, for both ABIs.
# Each .fake.self is staged here with its .fake.sprx, because RPCS3 mounts
# /app_home to the booted file's directory.

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    message(STATUS "sprx-segments: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

set(_sample "${CMAKE_CURRENT_SOURCE_DIR}/../../../samples/sprx/module-segments")
if(NOT EXISTS "${_sample}/CMakeLists.txt")
    message(FATAL_ERROR
        "sprx-segments: expected the sample at ${_sample}.\n"
        "  This probe builds the sample rather than copying it; if it "
        "moved, fix the path here rather than duplicating the sources.")
endif()

add_subdirectory("${_sample}" "${CMAKE_CURRENT_BINARY_DIR}/module-segments")

set(_out "${CMAKE_CURRENT_BINARY_DIR}/module-segments")
add_custom_target(sprx-segments ALL
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_out}/sprx_segments.fake.sprx" "${CMAKE_CURRENT_SOURCE_DIR}/sprx_segments.fake.sprx"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_out}/sprx-segments-loader.fake.self" "${CMAKE_CURRENT_SOURCE_DIR}/sprx-segments.fake.self"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_out}/sprx_segments_lp64.fake.sprx" "${CMAKE_CURRENT_SOURCE_DIR}/sprx_segments_lp64.fake.sprx"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_out}/sprx-segments-loader-lp64.fake.self" "${CMAKE_CURRENT_SOURCE_DIR}/sprx-segments-lp64.fake.self"
    DEPENDS sprx-segments-module sprx-segments-loader
            sprx-segments-module-lp64 sprx-segments-loader-lp64
    COMMENT "sprx-segments: staging the modules beside their loaders"
    VERBATIM)
