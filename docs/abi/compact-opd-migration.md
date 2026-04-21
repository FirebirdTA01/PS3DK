# Phase 3d.2 — True 8-byte `.opd` emission

## Goal

Make the PPU toolchain emit 8-byte compact function descriptors
natively — the layout the reference SDK uses and the Lv-2 ABI spec
(docs/abi/cellos-lv2-abi-spec.md §2) normatively requires. When this
phase is done:

- `.opd` entries are **8 bytes**: `[entry_ea_32, tlsgd_marker_32]`.
- `.opd` section size is `8 × n_functions` instead of `24 × n_functions`.
- `.rela.opd` uses `R_PPC64_ADDR32 + R_PPC64_TLSGD` per entry, not
  three `R_PPC64_ADDR64`.
- `tools/sprx-linker`'s `.opd` packing step is unnecessary and retired.
- `lv2_fn_to_callback_ea(fn)` becomes a bare cast — the `+16`
  offset goes away in a single edit.
- `__get_opd32` vanishes from every upstream fork we build against.

This doc captures why Phase 3d.2 is its own coordinated piece of
work (not just a binutils patch), what each piece looks like, and
what the validation matrix is.

---

## Why it is not a single patch

`samples/toolchain/hello-ppu-abi-check/hello-ppu-abi-check.elf`
disassembles to the standard PPC64 ELFv1 indirect-call sequence:

```asm
ld    r10, 0(r9)      ; entry EA from desc+0  (8 bytes)
ld    r11, 16(r9)     ; env slot from desc+16 (8 bytes)
mtctr r10
ld    r2,  8(r9)      ; TOC from desc+8       (8 bytes)
bctrl
```

Three loads spanning bytes 0-23 of the descriptor. If the linker
shrinks `.opd` entries to 8 bytes post-layout, `ld r2, 8(r9)` reads
**into the next descriptor** — every function-pointer call in the
tree breaks. Shrinking `.opd` therefore requires the compiler to
emit a different indirect-call sequence first.

This is the key reason Phase 3d.2 is *two* coordinated patches
(GCC and binutils) rather than one, and why it cannot be a purely
post-link rewrite.

---

## Workstreams

### A. GCC — indirect-call sequence change

**Target file(s):** `gcc/config/rs6000/rs6000*.{c,cc,md}`, specifically
whichever routine expands indirect calls under the AIX/ELFv1 path.
Candidates to inspect: `rs6000_call_aix`, `rs6000_indirect_call_sequence`,
`rs6000_expand_call`, and the `call_indirect_aix` /
`call_value_indirect_aix` patterns.

**Proposed sequence for Lv-2 mode** (replaces the 3-read form):

```asm
ld    r11, 0(r9)      ; entry EA (only read we need)
mtctr r11
bctrl
```

Single 8-byte read. No TOC reload — Lv-2 convention is that all
functions within a module share the module TOC, which is already
in r2 on entry, so indirect calls never need to switch TOC.

**Gating:** new target flag `-mps3-opd-compact` (default off
while developing, eventually default for the `powerpc64-ps3-elf`
target). Matches the `-mps3-runtime=native` pattern from Phase 2.

**Cross-module indirect calls:** require more care. Possible
answers:

1. **Stubs** — keep the existing `.sceStub.text` model; indirect
   calls to external functions get rewritten to go through a stub
   that sets r2 before branching. PSL1GHT already generates these
   for direct calls; extending them is straightforward.
2. **Banned** — declare cross-module indirect calls unsupported at
   source level. Lv-2 homebrew rarely takes the address of an
   imported symbol and calls through it; worth a grep across the
   PSL1GHT sample corpus to verify.
3. **Per-call TOC load** — two-read sequence: `ld r11, 0; ld r2, 4;
   mtctr r11; bctrl`. Requires the 8-byte descriptor's second word
   to hold a TOC EA for callees that need one — currently it
   carries a TLSGD marker (purpose still not fully resolved —
   see §3 of the main spec).

Recommended: start with (2) + (1) as fallback. Revisit (3) if
any sample genuinely needs it.

### B. GCC — static function-pointer storage

Some C constructs lay down function descriptors (or references to
them) in data sections:

- `.init_array`, `.fini_array`, `.ctors`, `.dtors` — constructor /
  destructor arrays. Each entry is traditionally an 8-byte pointer
  to a `.opd` entry.
- `void (*arr[])() = { a, b, c };` — C-level function-pointer
  tables. Each entry is an 8-byte pointer.
- `.eh_frame` / `.gcc_except_table` FDE personality routines —
  some back-ends emit a function-descriptor reference.

In compact mode these stay 8-byte pointers, but the *target* they
point at is now an 8-byte descriptor rather than a 24-byte one.
The compiler change is mostly about where those pointers are
resolved — the linker will still place them at correct offsets
given correct `.opd` layout. Audit required; not necessarily
additional compiler work.

### C. Binutils — `.opd` emission + reloc rewrite

**Target file:** `bfd/elf64-ppc.c`. The ppc64 backend already has
an `elf_backend_early_size_sections` hook (`ppc64_elf_edit`) that
does opd optimization; compact-opd emission is a natural addition
to the same pipeline.

**Transformation:**

For every input `.opd` section (24-byte entries, 3×ADDR64
relocations per entry):

1. Identify each 24-byte entry and its three relocations:
   `R_PPC64_ADDR64 @ +0  →  .funcname`
   `R_PPC64_ADDR64 @ +8  →  .TOC.@tocbase`
   `R_PPC64_ADDR64 @ +16 →  <env, usually 0>`
2. Emit an 8-byte entry:
   `R_PPC64_ADDR32 @ +0  →  .funcname` (truncate the +0 dword)
   `R_PPC64_TLSGD  @ +4  → no symbol, addend 0`
3. Drop the other two relocations.
4. Output section size = `n_entries × 8`.

**Relocation-of-references:** every relocation referring to
`.opd + N×24` in the input needs its addend recomputed as `N×8`
in the output. Sources include:

- `.toc` / `.got` entries holding function-descriptor pointers.
- `.init_array` / similar static tables.
- Any code-embedded constants (rare under -fPIC / -fPIE but
  possible).

The ppc64 backend already walks these during link via
`ppc64_elf_edit_toc`; hooking the compact-opd adjust into the
same iteration is natural.

**Gating:** new linker emulation / option.
`ld -m elf64lv2ppc` or `--ps3-compact-opd`. Same default-off /
default-on story as the GCC flag.

### D. `tools/sprx-linker` — stop packing

When binutils emits compact OPDs, the 24-byte descriptors don't
exist and the "pack 8 bytes at offset +16" step is a no-op (or
worse, clobbers the TLSGD marker slot). The tool's existing
8-byte-entry guard (added in the Phase 2.1 fork) already
bypasses the packing when descriptors are small; nothing to do
here beyond re-verification on a sample that uses the new path.

### E. Runtime / header cleanup

- Retire `lv2_fn_to_callback_ea`'s `+16` body — it becomes a bare
  cast `(lv2_ea32_t)(uintptr_t)fn`. All nine current call sites
  keep the same source, get new correct behaviour for free.
- Update `docs/abi/cellos-lv2-abi-spec.md §2` to remove the
  "target state / current output 24-byte" status note.
- Update `abi-verify` to handle linked EXEC/DYN `.opd` entries by
  reading bytes (not relocations, which are resolved), verifying
  8-byte stride.

---

## Validation matrix

Phase 3d.2 ships only when all of:

1. **Static conformance:** `abi-verify check` reports 8-byte `.opd`
   entries on a built sample, matching the fixture shape for
   `crt1.o` / `libc.a(exit.o)` from `tests/abi/fixtures/`.
2. **Linker structural check:** `powerpc64-ps3-elf-objdump -d`
   shows indirect calls using the 1-read sequence (not 3-read).
3. **Sample smoke:** every sample in `samples/` listed in
   `feedback_retest_samples_after_rebuild.md` builds green AND
   runs in RPCS3 to clean exit.
4. **Callback correctness:** `samples/gcm/hello-ppu-cellgcm-clear`
   continues to render its 6-color 360-frame cycle under
   compact-opd mode. (This sample exercises the VBlank handler —
   the path that `__get_opd32` used to cover.)
5. **No regressions in existing tests:** `hello-ppu-abi-check`
   still passes 8/8 `abi-verify` invariants.

---

## Risk register

| Risk | Likelihood | Mitigation |
|---|---|---|
| GCC's indirect-call change breaks same-module calls that *do* need a different TOC (e.g., weak-linked externals) | Medium | Start with stubs for externals, keep direct-jump for locals. Linker already distinguishes. |
| Static `.init_array` entries break because the embedded pointer targets are 24-byte old layout | Medium | Audit + explicit test sample with a C++-style global constructor. |
| The `R_PPC64_TLSGD` marker's actual loader semantics are still unknown — maybe it writes something specific we don't understand | High | Dump linked reference `.self` to see what bytes end up at +4 post-load. May need to write a specific value (e.g., module TOC EA) rather than leave it zero. |
| Cross-PRX function-pointer calls (theoretical) break silently | Low | Grep PSL1GHT samples + reference headers for patterns that take `&extern_func`. |
| EH unwinding + `.eh_frame` FDE personality references assume 24-byte FD layout | Medium | Build one C++ sample with exceptions; verify `catch` still unwinds. |

---

## Non-goals (for this phase)

- Shrinking `.toc` or `.got` layouts. They hold 8-byte pointers
  regardless of descriptor format; only the values change.
- Retiring PSL1GHT's `gcmConfiguration` (still needed for legacy
  callers; not connected to `.opd` format).
- Retiring GCC patch 0005 (`TARGET_VALID_POINTER_MODE`). That's
  driven by `mode(SI)` usage in PSL1GHT headers; orthogonal to
  descriptor shape.

---

## Estimated scope

Based on touchpoints and the need to land GCC + binutils patches
that co-operate:

- GCC patch (indirect-call sequence + target flag): ~300-500 LOC,
  one real patch file. Risk: moderate — the ppc64 backend is
  intricate.
- Binutils patch (compact-opd linker path): ~400-700 LOC, one real
  patch file. Risk: moderate — ppc64 edit code is dense but
  well-structured.
- Test samples: one minimal compact-opd sample (`hello-ppu-compact-opd`).
  Audit of existing samples: likely all pass without source changes.
- RPCS3 validation round: entire sample matrix re-booted.

Together: a dedicated multi-day session, not an afternoon. Phase
3d.1 (typed helper) is the stopover that lets us ship the
vocabulary cleanup immediately while this phase incubates.
