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
# RPCS3 mounts /app_home to the booted file's directory, so each boot file is
# staged in a directory holding only the module container it must load
# (issue #20):
#
#   ./      <row>.fake.self   + <stem>.fake.sprx   rows sprx-segments[-lp64]
#   raw/    <row>.elf         + <stem>.prx         rows sprx-segments-raw[-lp64]
#   real/   <row>.self        + <stem>.sprx        rows sprx-segments-real[-lp64]
#
# The loader tries .sprx, .fake.sprx and .prx in turn and prints the path it
# loaded; each row requires its own container's path.

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
set(_dst "${CMAKE_CURRENT_SOURCE_DIR}")
set(_stage)
foreach(_sfx "" "-lp64")
    string(REPLACE "-" "_" _msfx "${_sfx}")
    set(_stem "sprx_segments${_msfx}")
    set(_ldr "sprx-segments-loader${_sfx}")
    set(_row "sprx-segments${_sfx}")
    list(APPEND _stage
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_out}/${_stem}.fake.sprx" "${_dst}/${_stem}.fake.sprx"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_out}/${_ldr}.fake.self" "${_dst}/${_row}.fake.self"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_out}/${_stem}.prx" "${_dst}/raw/${_stem}.prx"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_out}/${_ldr}.elf" "${_dst}/raw/${_row}.elf"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_out}/${_stem}.sprx" "${_dst}/real/${_stem}.sprx"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_out}/${_ldr}.self" "${_dst}/real/${_row}.self")
endforeach()
add_custom_target(sprx-segments ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dst}/raw" "${_dst}/real"
    ${_stage}
    DEPENDS sprx-segments-module sprx-segments-loader
            sprx-segments-module-lp64 sprx-segments-loader-lp64
    COMMENT "sprx-segments: staging each container beside its loader"
    VERBATIM)
