cmake_minimum_required(VERSION 3.20)

project(tls_regression C)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    # This probe is specifically about PS3 PPU TLS codegen and loader setup.
    # Host compilers either reject __thread in this form or prove the wrong
    # runtime, so keep host-stub CI silent.
    message(STATUS "tls: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

set(_sample "${CMAKE_CURRENT_SOURCE_DIR}/../../../samples/lv2/hello-ppu-tls")
if(NOT EXISTS "${_sample}/CMakeLists.txt")
    message(FATAL_ERROR
        "tls: expected the shipped sample at ${_sample}.\n"
        "  This probe builds the sample rather than copying it; if it moved, "
        "fix the path here rather than duplicating the source.")
endif()

add_subdirectory("${_sample}" "${CMAKE_CURRENT_BINARY_DIR}/hello-ppu-tls")

set(_self "${_sample}/hello-ppu-tls.fake.self")

add_custom_target(tls ALL
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_self}" "${CMAKE_CURRENT_SOURCE_DIR}/tls-test.fake.self"
    DEPENDS hello-ppu-tls
    COMMENT "tls: staging the signed SELF for the regression harness"
    VERBATIM)
