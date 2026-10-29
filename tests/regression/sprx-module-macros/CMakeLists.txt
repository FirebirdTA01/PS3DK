cmake_minimum_required(VERSION 3.20)

project(sprx_module_macros_regression C ASM)

# Guards the SPRX path end to end: build a module using SYS_MODULE_* macros,
# sign it, load it at runtime, verify start handler execution and handoff,
# invoke exports via handoff and stub archive, verify stop handler run, and
# unload cleanly.

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    message(STATUS "sprx-module-macros: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

set(_sample "${CMAKE_CURRENT_SOURCE_DIR}/../../../samples/sprx/module-export")
if(NOT EXISTS "${_sample}/CMakeLists.txt")
    message(FATAL_ERROR
        "sprx-module-macros: expected the sample at ${_sample}.\n"
        "  This probe builds the sample rather than copying it; if it "
        "moved, fix the path here rather than duplicating the sources.")
endif()

add_subdirectory("${_sample}" "${CMAKE_CURRENT_BINARY_DIR}/module-export")

set(_module "${CMAKE_CURRENT_BINARY_DIR}/module-export/sample_module_export.fake.sprx")
set(_loader "${CMAKE_CURRENT_BINARY_DIR}/module-export/sample-module-loader.fake.self")

add_custom_target(sprx-module-macros ALL
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_module}" "${CMAKE_CURRENT_SOURCE_DIR}/sample_module_export.fake.sprx"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_loader}" "${CMAKE_CURRENT_SOURCE_DIR}/sprx-module-macros.fake.self"
    DEPENDS sample-module-export sample-module-loader
    COMMENT "sprx-module-macros: staging the signed module beside its loader"
    VERBATIM)
