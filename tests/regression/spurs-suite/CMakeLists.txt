cmake_minimum_required(VERSION 3.20)
project(spurs_suite_regression C CXX ASM)
option(PS3TC_REGRESSION_HOST_STUB "Host cannot run SPU code" OFF)
if(PS3TC_REGRESSION_HOST_STUB)
    return()
endif()
include(ps3-self)

# SPURS regression suite: one self per row, each printing
#   SPURS_SUITE <row> OK | FAIL <step> ... | INVALID <step> ...
# Rows cover the SPU task services, the SPU synchronisation objects and each
# SPU driver mode (manual task link, -mspurs-task, -mspurs-job,
# -mspurs-job-initialize), each driver mode against the same code linked by
# hand.  New SPURS services get a row here as they land.
# Each row is its own subdirectory: SPU image outputs are keyed on the image
# NAME inside a binary dir.
set(SUITE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT PS3_SELF_OUTPUT_DIRECTORY)
    set(PS3_SELF_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
include(${SUITE_DIR}/suite.cmake)
foreach(_row IN LISTS SUITE_ROW_DIRS)
    add_subdirectory(rows/${_row})
endforeach()
