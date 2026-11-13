# Profiler format allocations

This is the allocation registry. A reservation does not claim that its writer,
reader or target behavior is implemented. Chunk types and in-chunk record kinds
are different namespaces.

| File flag | Family | Chunk types |
|---|---|---|
| none | Base capture | 1 HEADER, 2 CLOCKS, 3 DESCRIPTORS, 4 THREADS, 5 RECORDS, 6 TRAILER, 7 RETIRED |
| bit 0 (1) | Domain extensions | 8 DOMAIN, 9 EPOCH, 10 CALIBRATION, 11 SPU_IMAGES, 12 SPU_LANES, 13 SPU_EVENTS, 14 GPU_EVENTS, 15 LOSS, 16 DOMAIN_FINAL |
| bit 1 (2) | Profile identity | 17 PROFILE |
| bit 2 (4) | SPU order | 18 SPU_ORDER |
| bit 3 (8) | Frame/source evidence | 19 FRAME_MEMBERSHIP, 20 FRAME_CLOSE, 21 FRAME_SOURCE_CHECKPOINT, 25 ADMISSION, 26 SOURCE_SEAL |
| bit 4 (16) | Pthread waits | 22 WAIT_CONFIG, 23 WAIT_RECORDS, 24 WAIT_STATUS |
| bit 5 (32) | Function paths | 28 FUNC_CONFIG, 29 FUNC_TREE, 30 FUNC_STATUS |
| bit 6 (64) | FIFO phase observation | 31 FIFO_CONFIG, 32 FIFO_EVENTS, 33 FIFO_STATUS |
| bit 7 (128) | Automatic RSX observation, reserved | 34 AUTO_CONFIG, 35 AUTO_OPS, 36 AUTO_COVERAGE, 37 AUTO_STATUS, 38 AUTO_CLOCK |

Chunk 27 remains reserved for the scope declaration under the existing frame
evidence capability. It must not be reused. Chunk 39 onward and flag bit 8 onward
are not allocated by this document.

Flag prerequisites: bit 1 requires bit 0; bit 2 requires bit 1; bit 6 requires
bit 0; bit 7 requires bit 0. There is no wire prerequisite from bit 7 to bit 6.
The automatic product may select both, but the families are independently
identifiable. Partial-header checks and full-header checks must agree.

The FIFO and automatic allocations were confirmed together on 2026-11-10.
At the host-model base, the reader implements bits 0 through 6 and chunks
1 through 26 and 28 through 33. It still refuses the reserved automatic family.
Allocation here is not reader acceptance or a console qualification.
