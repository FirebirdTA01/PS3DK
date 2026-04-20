# SPU Backend: GCC 9.5.0 Now, GCC 12+ Later

## Current state

The SPU toolchain uses **GCC 9.5.0** — the last GCC release with an intact Cell SPE
backend. This gives us:

- Full C++17, partial C++20
- All SPU intrinsics (`spu_*`, `mfc_*`, `si_*`)
- SPU-specific optimizations (`-mdual-nops`, `-mwarn-reloc`, `-fschedule-insns`)
- Software-managed cache support via libgcc (`libgcc_cachemgr.a`)

For typical SPU workloads (256 KB local store, tight computational kernels), GCC 9.5.0
is more than adequate. The SPU doesn't benefit from GCC 12's improved autovectorization
or LTO as much as PPU does.

## Long-term goal: Forward-port SPU backend to GCC 12+

GCC 10.1 (released May 2020) removed the SPU backend entirely:

> Remove support for the SPU target (Cell Broadband Engine synergistic
> processor unit), which has not been maintained for some time.
> — GCC 10.1 Release Notes

This deletion removed approximately **34,000 lines** across:

| Directory | Lines | What |
|---|---:|---|
| `gcc/config/spu/` | ~6,800 | Backend: machine description, target hooks, constraints, builtins |
| `libgcc/config/spu/` | ~3,200 | Runtime: divmod, float conversion, cache manager, MFC tag mgmt |
| `gcc/config/spu/*.h` | ~2,500 | Intrinsic headers: `spu_intrinsics.h`, `spu_mfcio.h`, etc. |
| Various config files | ~500 | `config.gcc`, `config.host`, `configure.ac` wiring |
| Test suite | ~21,000 | `gcc.target/spu/` tests |

### What a forward-port involves

1. **Resurrect `gcc/config/spu/`** from GCC 9.5.0 into GCC 12's tree
2. **Adapt to GCC 12 middle-end API changes** (significant):
   - `rs6000.c` → split into `rs6000.cc`, `rs6000-logue.cc`, `rs6000-call.cc`, etc.
   - Target hook API changes (`targetm` struct evolution)
   - New pass infrastructure (IPA, RTL)
   - C++ source file rename (`.c` → `.cc`)
   - `machine_mode` → `scalar_int_mode` / `scalar_float_mode` API tightening
3. **Update `spu.md`** machine description for any new RTL patterns
4. **Re-integrate libgcc/config/spu/** with GCC 12's libgcc build system
5. **Validate** the intrinsic headers still produce correct SIMD code
6. **Re-enable test suite** under GCC 12's testing infrastructure

### Estimated effort

**12-24 weeks** of focused compiler engineering, depending on:
- Depth of middle-end API drift between GCC 9 and 12
- Whether we attempt GCC 13+ (even more API churn) or stop at 12
- How much of the test suite we want green

### Why bother?

- **Unified toolchain version**: one GCC for both PPU and SPU simplifies CI/CD
- **Full C++20 on SPU**: concepts, ranges, coroutines (where local store permits)
- **Better optimizations**: GCC 12 has improved instruction scheduling, loop optimization
- **Maintainability**: tracking one GCC version instead of two

### Approach

This work will be done in a **separate worktree/branch** (`spu-gcc12-forward-port`)
to avoid disrupting the working GCC 9.5.0 SPU toolchain. The two can coexist:

```
stage/ps3dev/spu/       ← GCC 9.5.0 (production, works now)
stage/ps3dev/spu-gcc12/ ← GCC 12.4.0 (experimental, when ready)
```

### Reference materials

- GCC 9.5.0 `gcc/config/spu/` — the source of truth for the backend
- GCC 10.1 removal commit: search for "Remove SPU" in GCC git log
- Cell BE Architecture documents (see the public CBEA Handbook / SPU ISA PDFs from IBM)
- SPU ISA v1.2 (IBM document)
- `gcc/doc/tm.texi` — target macros reference for GCC 12
