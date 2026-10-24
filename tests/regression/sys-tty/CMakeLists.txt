cmake_minimum_required(VERSION 3.20)

project(sys_tty_regression C)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    # The row needs the lv2 TTY system call.
    message(STATUS "sys-tty: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

include(ps3-self)

add_executable(sys-tty source/main.c)
target_compile_options(sys-tty PRIVATE -Wall -Wextra -Werror)
ps3_add_self(sys-tty)
