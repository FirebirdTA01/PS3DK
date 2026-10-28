# Full-size PSGL readback

This row compares every RGBA byte in full default-buffer reads after red and
blue clears, plus 1280x720 and 1920x1080 reference-texture framebuffers.
`GL_STATIC_DRAW` exercises local-to-main transfer; `GL_SYSTEM_DRAW_SCE`
exercises main-to-main transfer. The system fixture has four distinct
colors separated at storage x/y 1024, with X=0x13 to require alpha=255.
These location labels follow the implementation's usage-to-storage mapping;
the test does not independently query the allocated buffer's location.
The 1080p cases also read (1001,23), width 81 and height 1041: a nonzero
origin across both transfer boundaries, with four untouched padding bytes
per output row at pack alignment 8. Front/back guards cover every read.

A test-only linker wrapper observes `memalign` and forwards to the real
allocator. Each case prints the measured scratch request and checks exactly
one allocation: 4 MiB for 720p, 8 MiB for 1080p, and 1 MiB for the offset
rectangle. The default-buffer expectation follows the queried display size;
this row does not change the display mode. Volatile fixture writes avoid
coupling DMA expectations to the separately investigated GCC16 vectorizer
row-initializer defect.

Both ABIs must report `PSGL_READ_FULL_OK checks=62 failures=0` and exit 0.
No shaders or asynchronous draw retries are needed. These are synchronous
transfer/correctness checks, not hardware or throughput acceptance.
