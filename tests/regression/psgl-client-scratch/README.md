# Client-array scratch lifetime

Eight rows cover wrap, growth, wrap timeout and growth timeout in both PPU
ABIs. Each queues a red client-array draw behind a GPU label, then attempts
a blue upload that needs to recycle the scratch allocation. A PPU thread
releases the gate after 500 ms. Timeout rows use five sequential gates,
released 500 ms apart, so each wait stays short while the total exceeds
the two-second completion bound. A single long semaphore wait can trigger
emulator recovery before the completion helper times out.

The success rows require completion before recycling and exact red/blue
pixels. Timeout rows require `GL_OUT_OF_MEMORY`, no second draw, preserved
red pixels and a successful retry after completion. Each row has 12 checks.
Shader warm-up is bounded and precedes the gated draw.

The host test `tests/host/psgl-client-scratch-test.sh` compiles the production
allocator against a simulated queued reader. It also injects unmap, allocation
and mapping failures, checking storage lifetime and retry behavior. RPCS3
pixel checks alone cannot prove that freed storage remained mapped correctly.

Build using the PPU CMake toolchain. Optional `PSGL_ARCHIVE_ilp32` and
`PSGL_ARCHIVE_lp64` paths select private archives; otherwise the installed
PSGL libraries are used. These rows do not establish hardware behavior or
general GL conformance.
