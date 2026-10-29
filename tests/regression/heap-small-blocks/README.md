# Small heap blocks, both PPU pointer ABIs

Runs the same ten cases as `tests/host/heap-small-block-test.py` against the
target allocator: isolated eight-byte frees, sizes 1 through 40, all six free
orders for three adjacent blocks, and deterministic aligned/unaligned churn.
Checks live payloads, free-list links, and full coalescing after cleanup.

Build with a private candidate runtime and the PPU CMake toolchain. Each ABI
must emit `HEAP_SMALL_OK tests=10 pass=10 fail=0` and exit cleanly. Before the
minimum-block fix, LP64 fails the free-list checks; ILP32 is the control.
