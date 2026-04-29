# hello-spu-dma — exercise the SPU-side <cell/dma.h> wrappers.
#
# PPU prepares an input buffer + output buffer in main memory and an EA
# table that includes both buffers and a "done" flag.  An embedded SPU
# program is spawned which:
#   - cellDmaGet's the input buffer into LS
#   - cellDmaListGet's the same buffer through a 2-element list
#     (split fetch) to exercise the list path
#   - doubles every uint32 element
#   - cellDmaPut's the result back to the output buffer
#   - cellDmaWaitTagStatusAll's after every transfer
#   - writes a sentinel through DMA to the done flag and exits
# The PPU verifies the output and prints PASS/FAIL.
#
# Validates: cellDmaGet, cellDmaPut, cellDmaListGet,
#            cellDmaWaitTagStatusAll, CellDmaListElement layout.

cmake_minimum_required(VERSION 3.20)
project(hello-spu-dma C ASM)

include(ps3-self)

add_executable(hello-spu-dma source/main.c)
target_compile_features(hello-spu-dma PRIVATE c_std_11)

target_link_libraries(hello-spu-dma PRIVATE
    rt lv2 m
)

ps3_add_spu_image(hello-spu-dma
    NAME hello_spu_dma
    SOURCES spu/source/main.c
    LIBS sputhread)

ps3_add_self(hello-spu-dma)
