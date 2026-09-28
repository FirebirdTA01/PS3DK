cmake_minimum_required(VERSION 3.20)
project(spurs_event_flag_regression CXX C ASM)
option(PS3TC_REGRESSION_HOST_STUB "Host cannot run SPU code" OFF)
if(PS3TC_REGRESSION_HOST_STUB)
    return()
endif()
include(ps3-self)
# The SPU side of the SPURS event flag (libspurs_task) against the firmware
# SPURS kernel: PPU2SPU, SPU2SPU and SPU2PPU, each forced onto its blocking
# path.  The SPU program is a SPURS task (spurs_task.ld, no startfiles).
add_executable(spurs-event-flag main.cpp)
target_compile_features(spurs-event-flag PRIVATE cxx_std_17)
target_compile_options(spurs-event-flag PRIVATE -Wall -Wextra)
target_link_libraries(spurs-event-flag PRIVATE spurs_stub c_stub sysmodule_stub sysutil rt lv2 m)
ps3_add_spu_image(spurs-event-flag
    NAME spu_task
    SOURCES spu/task.c
    CFLAGS -O2 -Wall -Wextra -Werror -Wno-cpp
    NOSTARTFILES
    FREESTANDING
    LDSCRIPT ${PS3DK}/spu/ldscripts/spurs_task.ld
    LIBS spurs_task)
ps3_add_self(spurs-event-flag)
