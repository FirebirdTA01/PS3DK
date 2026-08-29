cmake_minimum_required(VERSION 3.20)

project(sprx_load_regression C ASM)

# Guards the SPRX path end to end: build a module, sign it, load it at runtime,
# resolve its exports by NID and call through both the start-time hand-off and
# the generated stub archive.
#
# This probe deliberately does NOT carry its own copy of the sources. It pulls
# in the shipped samples with add_subdirectory, so it exercises the same
# CMakeLists a user builds -- including whatever arguments ps3_add_prx is
# actually called with. A copied probe would let the samples rot while the
# regression still passed, which is the opposite of what a battery is for.
#
# The importer boots as .fake.self, so it loads the .fake.sprx: a module's
# container has to match the executable's. Both are staged into this directory
# because RPCS3 mounts /app_home to the booted file's directory.

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    # The SPRX path needs the cross toolchain and the host PRX tools; there is
    # nothing meaningful to stub on a host build. Stay silent rather than
    # pretend to cover it.
    message(STATUS "sprx-load: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

set(_samples "${CMAKE_CURRENT_SOURCE_DIR}/../../../samples/lv2")
foreach(_dir hello-sprx-export hello-sprx-import)
    if(NOT EXISTS "${_samples}/${_dir}/CMakeLists.txt")
        message(FATAL_ERROR
            "sprx-load: expected the shipped sample at ${_samples}/${_dir}.\n"
            "  This probe builds the samples rather than copying them; if they "
            "moved, fix the path here rather than duplicating the sources.")
    endif()
endforeach()

add_subdirectory("${_samples}/hello-sprx-export" "${CMAKE_CURRENT_BINARY_DIR}/hello-sprx-export")
add_subdirectory("${_samples}/hello-sprx-import" "${CMAKE_CURRENT_BINARY_DIR}/hello-sprx-import")

# ps3_add_prx / ps3_add_self write their artefacts next to the sample's own
# CMakeLists, so stage the pair here where the harness and /app_home expect it.
set(_module "${_samples}/hello-sprx-export/hello-sprx-export.fake.sprx")
set(_loader "${_samples}/hello-sprx-import/hello-sprx-import.fake.self")

add_custom_target(sprx-load ALL
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_module}" "${CMAKE_CURRENT_SOURCE_DIR}/hello-sprx-export.fake.sprx"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_loader}" "${CMAKE_CURRENT_SOURCE_DIR}/sprx-load.fake.self"
    DEPENDS hello-sprx-export hello-sprx-import
    COMMENT "sprx-load: staging the signed module beside its loader"
    VERBATIM)
