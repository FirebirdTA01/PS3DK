cmake_minimum_required(VERSION 3.20)

project(pthread_sync_regression C)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    # This probe targets the PS3DK pthread shim over Lv-2 primitives.
    # Host pthreads would prove the wrong runtime.
    message(STATUS "pthread-sync: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

set(_sample "${CMAKE_CURRENT_SOURCE_DIR}/../../../samples/lv2/hello-ppu-pthread")
if(NOT EXISTS "${_sample}/CMakeLists.txt")
    message(FATAL_ERROR
        "pthread-sync: expected the shipped sample at ${_sample}.\n"
        "  This probe builds the sample rather than copying it; if it moved, "
        "fix the path here rather than duplicating the source.")
endif()

add_subdirectory("${_sample}" "${CMAKE_CURRENT_BINARY_DIR}/hello-ppu-pthread")

set(_self "${_sample}/hello-ppu-pthread.fake.self")

add_custom_target(pthread-sync ALL
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_self}" "${CMAKE_CURRENT_SOURCE_DIR}/pthread-sync.fake.self"
    DEPENDS hello-ppu-pthread
    COMMENT "pthread-sync: staging the signed SELF for the regression harness"
    VERBATIM)
