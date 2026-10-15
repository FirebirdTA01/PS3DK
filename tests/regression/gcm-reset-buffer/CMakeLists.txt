cmake_minimum_required(VERSION 3.20)

project(gcm_reset_buffer_regression C)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    # The probe needs a real RSX FIFO.
    message(STATUS "gcm-reset-buffer: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

include(ps3-self)

add_executable(gcm-reset-buffer main.c)
target_compile_options(gcm-reset-buffer PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gcm-reset-buffer PRIVATE gcm_cmd gcm_sys rt lv2)
ps3_add_self(gcm-reset-buffer)
