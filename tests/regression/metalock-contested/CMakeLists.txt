cmake_minimum_required(VERSION 3.20)

project(metalock_contested_regression C)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    # This probe runs newlib's lv2 lock-creation code on PPU threads with lv2
    # priorities; a host build would test nothing of it.
    message(STATUS "metalock-contested: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

include(ps3-self)

# The code under test is newlib's lock_internal.c exactly as patch 0004 adds it.
# Extract it (and lock_internal.h) from the patch with string operations only:
# list operations would split the C source at its semicolons.
set(_patch "${CMAKE_CURRENT_SOURCE_DIR}/../../../patches/ppu/newlib-4.x/0004-newlib-libc-sys-lv2-reent-locks.patch")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_patch}")
file(READ "${_patch}" _text)
set(_out "${CMAKE_CURRENT_BINARY_DIR}/extracted")
foreach(_name lock_internal.c lock_internal.h)
    string(FIND "${_text}" "+++ b/newlib/libc/sys/lv2/${_name}\n" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "metalock-contested: ${_name} not found in ${_patch}")
    endif()
    string(SUBSTRING "${_text}" ${_at} -1 _rest)
    string(FIND "${_rest}" "\n@@ " _hunk)
    math(EXPR _hunk "${_hunk} + 1")
    string(SUBSTRING "${_rest}" ${_hunk} -1 _rest)
    string(FIND "${_rest}" "\n" _eol)
    math(EXPR _eol "${_eol} + 1")
    string(SUBSTRING "${_rest}" ${_eol} -1 _rest)
    string(FIND "${_rest}" "\ndiff --git" _end)
    if(NOT _end EQUAL -1)
        string(SUBSTRING "${_rest}" 0 ${_end} _rest)
    endif()
    string(REPLACE "\n+" "\n" _rest "\n${_rest}")
    string(SUBSTRING "${_rest}" 1 -1 _rest)
    file(WRITE "${_out}/${_name}" "${_rest}\n")
endforeach()

foreach(_abi ilp32 lp64)
    set(_t metalock-contested-${_abi})
    add_executable(${_t} main.c "${_out}/lock_internal.c")
    target_include_directories(${_t} PRIVATE "${_out}")
    # Test build of the library code: no start-up constructor, a delay hook in
    # the creator, and its own symbol so libc's copy is untouched.
    target_compile_definitions(${_t} PRIVATE
        LOCK_INTERNAL_TEST=1
        "LOCK_INTERNAL_TEST_AFTER_CLAIM()=test_after_claim()"
        __libc_auto_lock_allocate=test_auto_lock_allocate)
    target_compile_options(${_t} PRIVATE -O2 -include "${CMAKE_CURRENT_SOURCE_DIR}/hook.h")
    if(_abi STREQUAL "lp64")
        target_compile_options(${_t} PRIVATE -mlp64)
        target_link_options(${_t} PRIVATE -mlp64)
    endif()
    ps3_add_self(${_t})
endforeach()
