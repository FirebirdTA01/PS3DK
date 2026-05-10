# hello-ppu-mlp64-types

Probe sample.  Prints `sizeof` of every width-sensitive primitive plus the
state of `__LP64__` / `__ILP32__` macros.  Built twice from one source so
both ABI flavours ship side by side.

## Build flags

| Variant | Driver flag | Targets in this CMakeLists |
|---|---|---|
| ILP32 hybrid | (default) | `hello-ppu-mlp64-types-ilp32` |
| LP64         | `-mlp64`  | `hello-ppu-mlp64-types-lp64`  |

Both variants are produced by `cmake --build` automatically — running the
build produces the two `.fake.self` artefacts side by side.

> **LP64 runtime status (2026-05-09).**  The LP64 build artefact is
> produced at link time, but currently cannot reach `main()` at
> runtime: the multilib install tree uses ILP32-shape CRT objects and
> SPRX stub archives, and the resulting hybrid binary crashes at
> boot.  This is a known gap tracked in the multilib completion
> workstream.  Both ABIs remain supported targets; the LP64 path is
> in progress.

## Expected output

| Variant | `sizeof(void *)` | `sizeof(long)` | `sizeof(uintptr_t)` | `__LP64__` / `__ILP32__` |
|---|---|---|---|---|
| ILP32 hybrid | 4 | 4 | 4 | 0 / 1 |
| LP64         | 8 | 8 | 8 | 1 / 0 |
