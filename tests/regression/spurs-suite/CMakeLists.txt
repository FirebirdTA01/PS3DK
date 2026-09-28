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
# NAME inside a binary dir, and several rows embed an image named spu_task.
set(SUITE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT PS3_SELF_OUTPUT_DIRECTORY)
    set(PS3_SELF_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
set(SUITE_PPU_LIBS spurs_stub c_stub sysmodule_stub sysutil rt lv2 m)

function(suite_ppu target source)
    add_executable(${target} ${SUITE_DIR}/ppu/${source})
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_compile_options(${target} PRIVATE -Wall -Wextra "-DSUITE_ROW=\"${target}\"")
    target_link_libraries(${target} PRIVATE ${ARGN} ${SUITE_PPU_LIBS})
endfunction()

# SPURS tasks linked by hand: task startup, runtime and layout named
# explicitly; extra SPU libraries follow the source
function(suite_task_manual target spu_source)
    ps3_add_spu_image(${target}
        NAME spu_task
        SOURCES ${SUITE_DIR}/spu/${spu_source}
        CFLAGS -O2 -Wall -Wextra -Werror -Wno-cpp
        NOSTARTFILES
        FREESTANDING
        LDSCRIPT ${PS3DK}/spu/ldscripts/spurs_task.ld
        LIBS spurs_task ${ARGN})
endfunction()

add_subdirectory(rows/spurs-task-manual)
add_subdirectory(rows/spurs-task-driver)
add_subdirectory(rows/spurs-event-flag)
add_subdirectory(rows/spurs-event-flag-driver)
add_subdirectory(rows/spurs-queue)
add_subdirectory(rows/spurs-lfqueue)
add_subdirectory(rows/spurs-job-chain-manual)
add_subdirectory(rows/spurs-job-chain-driver)
add_subdirectory(rows/spurs-job-queue-manual)
add_subdirectory(rows/spurs-job-queue-driver)
