cmake_minimum_required(VERSION 3.20)
project(spu_celldma_regression C ASM)
option(PS3TC_REGRESSION_HOST_STUB "Host cannot run SPU code" OFF)
if(PS3TC_REGRESSION_HOST_STUB)
    return()
endif()
include(ps3-self)
# One PPU harness running one SPU program that drives each cellDma family
# (unaligned, large, small, scalar, lock-line, cancel) against PPU memory.
# The SPU links libdma for the large/unaligned splitters and cellDmaAndWait.
add_executable(spu-celldma main.c)
target_compile_features(spu-celldma PRIVATE c_std_11)
target_compile_options(spu-celldma PRIVATE -Wall -Wextra -Werror=implicit-function-declaration)
target_link_libraries(spu-celldma PRIVATE lv2)
ps3_add_spu_image(spu-celldma
    NAME spu
    SOURCES spu/main.c
    CFLAGS -O2 -Wall -Wextra -Werror
    LIBS dma)
ps3_add_self(spu-celldma)
