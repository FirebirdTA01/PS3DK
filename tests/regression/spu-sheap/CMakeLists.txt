cmake_minimum_required(VERSION 3.20)
project(spu_sheap_regression C ASM)
option(PS3TC_REGRESSION_HOST_STUB "Host cannot run SPU code" OFF)
if(PS3TC_REGRESSION_HOST_STUB)
    return()
endif()
include(ps3-self)

# Seven rows, each a PPU harness (ppu/<row>.c) embedding one SPU program
# (spu/<row>.c).  The SPU side links the SPU libsheap and what it uses;
# the PPU side reaches the firmware cellSheap / cellSync through the stubs.
#   sheap-p0-layout       firmware layout as the SPU library reads it
#   sheap-p1-spu          SPU-only heap
#   sheap-p2-shared       one heap used from the PPU (firmware) and an SPU
#   sheap-p3-contention   keyed objects, six SPUs + PPU on one mutex
#   sheap-allocate        allocate sample flow   (512th prime 3671)
#   sheap-key-buffer      key_buffer sample flow (64 rows of primes)
#   sheap-key-mutex       key_mutex sample flow  (ans => 4000)
set(SPU_SHEAP_ROWS
    "sheap-p0-layout:p0_layout:p0"
    "sheap-p1-spu:p1_spu:p1"
    "sheap-p2-shared:p2_shared:p2"
    "sheap-p3-contention:p3_contention:p3"
    "sheap-allocate:allocate:allocate"
    "sheap-key-buffer:key_buffer:key_buffer"
    "sheap-key-mutex:key_mutex:key_mutex")

foreach(row ${SPU_SHEAP_ROWS})
    string(REPLACE ":" ";" parts "${row}")
    list(GET parts 0 target)
    list(GET parts 1 source)
    list(GET parts 2 image)
    add_executable(${target} ppu/${source}.c)
    target_compile_features(${target} PRIVATE c_std_11)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror=implicit-function-declaration)
    target_link_libraries(${target} PRIVATE sheap_stub sync_stub sysmodule_stub lv2)
    ps3_add_spu_image(${target}
        NAME ${image}
        SOURCES spu/${source}.c
        CFLAGS -O2 -Wall -Wextra -Werror
        LIBS sheap sync dma atomic)
    ps3_add_self(${target})
endforeach()
