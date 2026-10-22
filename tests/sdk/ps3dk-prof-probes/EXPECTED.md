# Early standalone probes: PROBE-O-early and PROBE-T-early (TARGET-PLAN-r2 section 2, ordering step 1)

- **What these are:** sources and expected evidence for review only.
- **What they are not:** they are NOT wired into any build, CI job or test list. Compiling them needs its own frozen
  request.
- **Independence:** both are independent of `pal_lv2.c` and of the profiler objects.
- **What they can establish:** the compiler's lowering of the same builtins under the stated flags. The actual-object
  checks (PROBE-O-actual, PROBE-T-actual) come after pal_lv2 (ordering step 3).

## Build matrix (to be pinned by the request, not here)

- **Compiler:** GCC 16 `powerpc64-ps3-elf-gcc` only (R6). GCC 12.4 stays NOT RUN.
- **ABIs and flags:** ILP32 (default) and `-mlp64`, each at `-O2` and at the SDK's shipped library flags (taken from
  the build rule the request names).
- **Kept per (ABI, flags):**
  - the source;
  - the `-S` assembly;
  - for PROBE-T, also the final RTL dump (`-fdump-rtl-final`, or the dump the request names);
  - the compiler identity (path, version, sha256);
  - the installed header bytes used.

## PROBE-O-early: expected evidence, read per function (not grep)

| Function | Expected lowering (to be confirmed, not assumed) |
|---|---|
| `probe_o_release_store_u32` | The relaxed data store, then a `lwsync` (or `sync`), then the `stw` to `g32_flag`, with no store to `g32_flag` before the barrier. |
| `probe_o_release_store_u64` | The same with `std` to `g64_idx`. In ILP32 the 64-bit store is still ONE `std` (the object is 8-byte aligned), not two `stw`. |
| `probe_o_acquire_load_u32` | `lwz` of `g32_flag`, then the acquire sequence (for example `cmpw` + `bne-` + `isync`, or `lwsync`) BEFORE the dependent `lwz` of `g32_data`. |
| `probe_o_acquire_load_u64` | The same with ONE `ld` of `g64_idx`, in both ABIs. |
| `probe_o_cas_acqrel_u32` | A `lwarx`/`stwcx.` retry loop, preceded by the release barrier and followed by the acquire barrier on both the success and the failure path. |
| `probe_o_fetch_add_relaxed_u64` | A `ldarx`/`stdcx.` loop with no barrier (relaxed), as ONE 64-bit reservation in both ABIs. |

- **Results:** a written per-function table: function, instruction offsets and the instructions read.
- **FINDINGS:** any 64-bit atomic split into 32-bit halves, any call to `__atomic_*` (libatomic), or a missing barrier.
- **PROBE-L:** the two `_Static_assert`s compile with rc 0 in both ABIs.

## PROBE-T-early: expected evidence (three parts, per (ABI, flags))

1. **Source:** `probe_t_timebase_between_barriers` holds ACCESS-BEFORE, BARRIER-1, READ, BARRIER-2 and ACCESS-AFTER,
   in that order.
2. **RTL** (final dump):
   - The two barrier statements appear as volatile asm with a memory clobber.
   - The volatile store to `t_before` is before BARRIER-1.
   - The timebase read (`mftb`/`mfspr` 268/269, or the builtin's pattern) is between the barriers.
   - The load of `t_after_src` and the store to `t_after` are after BARRIER-2.
3. **Final assembly:**
   - The store to `t_before` precedes the timebase read sequence.
   - The load of `t_after_src` and the store to `t_after` follow it.
   - In ILP32 the read is one full-width `mftb`, or the documented TBU/TBL retry loop if the builtin lowers that way;
     either is recorded as found.

- **Control:** `probe_t_control_no_barriers` is recorded as found. Whatever order it shows is not a finding. It only
  shows that the barriers are the variable under test.
- **Stated guarantee:** NO hardware serialization and NO register-only-motion guarantee. The conclusion is bounded to
  this fixture, these call sites and these flags (codex 02:49).
