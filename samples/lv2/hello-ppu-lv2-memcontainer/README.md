# LV2 memory container

Creates a 2 MiB memory container and, one at a time, allocates a 512 KiB
region from it, checks the address is nonzero, writes and reads back the
region's first and last words, and frees it. All four cycles must complete,
and the container's total must be unchanged afterwards. It then destroys
the container and checks that allocating from or destroying an invalid
container fails and that `sys_memory_free(0)` returns EINVAL. The last line
is `result: PASS (n of m checks passed)` or `FAIL`; the exit status is 0 or 1.

## Build

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PS3DK/cmake/ps3-ppu-toolchain.cmake" \
  -DPS3_SELF_OUTPUT_DIRECTORY="$PWD/build"
cmake --build build
```

`ps3-ppu-toolchain-lp64.cmake` builds the LP64 variant.

## Limitations

Only sequential allocations are covered, not exhausting the container or
the exact available-size accounting. The error codes for the invalid
container calls are printed, not required. An allocation that reports
success with address 0 fails the run; `tests/regression/sample-trio-witness`
checks that with a faked allocation.
