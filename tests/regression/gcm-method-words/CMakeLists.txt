cmake_minimum_required(VERSION 3.20)

project(gcm_method_words_regression C)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    # The probe is about what the PPU gcm headers emit; a host build has no
    # gcm headers to test.
    message(STATUS "gcm-method-words: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

include(ps3-self)

add_executable(gcm-method-words main.c)
target_compile_options(gcm-method-words PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gcm-method-words PRIVATE gcm_cmd gcm_sys rt lv2)
ps3_add_self(gcm-method-words)
