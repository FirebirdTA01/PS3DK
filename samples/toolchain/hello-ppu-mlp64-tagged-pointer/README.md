# hello-ppu-mlp64-tagged-pointer

Demonstrates the high-bit pointer-tagging trick: pack a 16-bit type tag
into the unused high 16 bits of a pointer, recover it via mask + shift.
Common in 64-bit runtimes (V8, Mono, JVM, custom allocators) where the
canonical address space is 48 bits and the top 16 bits are free.

The trick requires `sizeof(uintptr_t) >= 8`, so it works under `-mlp64`
and does not work under default ILP32.  Both build variants ship from
this CMakeLists; the sample's runtime output is the demonstration.

## Build flags

| Variant | Driver flag | Targets in this CMakeLists |
|---|---|---|
| ILP32 hybrid | (default) | `hello-ppu-mlp64-tagged-pointer-ilp32` |
| LP64         | `-mlp64`  | `hello-ppu-mlp64-tagged-pointer-lp64`  |

## Expected output

| Variant | Result | Why |
|---|---|---|
| ILP32 hybrid | `FAIL` | `sizeof(uintptr_t) == 4`; the `<< 48` shift is undefined on a 32-bit type, the tag is dropped, unpack reads zero |
| LP64         | `PASS` | `sizeof(uintptr_t) == 8`; tag survives the round-trip |

> **LP64 runtime status (2026-05-09).**  The LP64 build artefact is
> produced at link time, but currently cannot reach `main()` at
> runtime: the multilib install tree uses ILP32-shape CRT objects and
> SPRX stub archives, and the resulting hybrid binary crashes at
> boot.  This is a known gap tracked in the multilib completion
> workstream.  Both ABIs remain supported targets; the LP64 path is
> in progress.
