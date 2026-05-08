# CellOS Lv-2 toolchain architecture

Companion to `docs/abi/cellos-lv2-abi-spec.md`.

The ABI spec defines what a conforming CellOS Lv-2 PPU binary must look like.
This document explains how the current PS3DK pieces cooperate to produce one,
and where PSL1GHT compatibility is still intentionally present.

The target is not stock PPC64 ELFv1. PS3DK's PPU userland target is a
big-endian ELF64/PPC64 environment with:

- `EI_OSABI = 0x66`;
- `e_flags = 0x00000000`;
- native 8-byte compact `.opd` function descriptors;
- a 64-byte `.sys_proc_prx_param` process metadata block;
- **ILP32 by default** — Pmode = SImode, 32-bit C pointers, 4-byte
  TOC slots (`R_PPC64_ADDR32`), `lwz @toc@l(r2)` loads — under a
  64-bit ELF wrapper.  `-mlp64` opt-in flips the multilib axis to
  64-bit pointers (legacy LP64 path) for downstream code that
  cannot be ported.  See `docs/abi/cellos-lv2-abi-spec.md §4`.
- explicit 32-bit EA typedef (`lv2_ea32_t`) for kernel-facing struct
  fields that the Lv-2 syscall layer treats as 32-bit EAs regardless
  of whether the user binary is ILP32 or LP64;
- a framed SPRX import-trampoline shape that protects the trampoline's
  caller-LR / caller-TOC saves from being clobbered by the SPRX
  function it calls into via `bctrl` (see ABI spec §5.1);
- NID/FNID-driven import metadata and stub archives, with PSL1GHT-style
  aliases folded into the canonical Sony nidgen output.

---

## Current status in one paragraph

The compact `.opd` ABI path is implemented: native output uses 8-byte
descriptors, compact indirect-call reads, and a bare `lv2_fn_to_callback_ea`
cast. The runtime layer has also replaced several PSL1GHT CRT pieces, including
`lv2-sprx.o`, `lv2-crti.o`, the `crt0.S` half of `lv2-crt0.o`, and `lv2.ld`.
However, the toolchain is not fully PSL1GHT-free yet: `lv2-crt0.o` still merges
in PSL1GHT's `crt1.o`, `sprx-linker` is still used for `.lib.stub` import
counts and legacy `.opd` fallback handling, and some SDK libraries remain
PSL1GHT-compatibility bridges.

---

## Layer-by-layer map

### 1. GCC — PPU target `powerpc64-ps3-elf`

Source: upstream GCC rs6000 backend plus PS3DK patches under
`patches/ppu/gcc-12.x/`.

Responsibilities:

- emit PPU code under the ELF64 + ILP32 hybrid pointer model
  (Pmode = SImode by default; multilib axis on `-mlp64` for the
  LP64 legacy path);
- emit native compact `.opd` descriptors for functions;
- emit the compact indirect-call sequence that reads two 32-bit words from a
  descriptor;
- emit 4-byte TOC slots (`.long sym` with `R_PPC64_ADDR32`) under
  Pmode = SImode, so a `lwz @toc@l(r2)` load reads the address
  directly (8-byte slots under big-endian put the address in the
  low half and the load returns zero — provoked Layer 11 when
  libgloss was built before the patch landed);
- spill / reload pointer-typed registers at 32-bit width
  (`stw r0, X(r1)` / `lwz r0, X(r1)`) under Pmode = SImode;
- provide PS3/Lv-2 target predefines used by SDK headers.

The compact descriptor path means GCC output is not stock PPC64 ELFv1 even
though it still uses the PPC64 backend lineage. Function pointers point at
8-byte descriptors, not 24-byte descriptors.

### 2. Binutils

Source: upstream binutils plus PS3DK patches under
`patches/ppu/binutils-2.42/`.

Responsibilities:

- assemble GCC and hand-written assembly into PPC64 big-endian REL objects;
- link objects, archives, CRT objects, and linker script output into a loadable
  ELF;
- write the CellOS Lv-2 ELF identity where required;
- resolve compact `.opd` descriptor fields to 32-bit entry and TOC EAs;
- avoid rewriting native compact `.opd` sections as stock 24-byte PPC64 ELFv1
  descriptors.

The current ABI spec requires `EI_OSABI = 0x66` and `e_flags = 0x00000000`.
Older docs and patches referred to `e_flags = 0x01000000`; that value is now
considered stale relative to the normative spec.

### 3. Newlib / libgloss / libsysbase

Source: upstream newlib plus PS3DK patches under `patches/ppu/newlib-4.x/`.

Responsibilities:

- provide `libc.a` and `libm.a`;
- provide the low-level syscall glue used by libc;
- provide reentrant lock glue for Lv-2 threading;
- route file, heap, TTY, and other libc operations through Lv-2-facing code.

Newlib does not define the Lv-2 executable ABI by itself. It runs inside the
environment established by GCC, binutils, the linker script, CRT objects, and
the SDK syscall/stub surface.

### 4. CRT and runtime layer

Source: `runtime/lv2/crt/`, with one remaining PSL1GHT object reused today.

Installed by `scripts/build-runtime-lv2.sh`.

Current installed pieces:

| Startfile / file | Current source | Status |
|---|---|---|
| `lv2-sprx.o` | `runtime/lv2/crt/lv2-sprx.S` | native PS3DK; emits 64-byte `.sys_proc_prx_param` and library-range prefixes |
| `lv2-crti.o` | `runtime/lv2/crt/lv2-crti.S` | native PS3DK compact-OPD init/fini prologue |
| `crt0.o` | `runtime/lv2/crt/crt0.S` | native PS3DK process entry and compact `__start` descriptor |
| `lv2-crt0.o` | `crt0.o` merged with PSL1GHT `crt1.o` | hybrid; still reuses PSL1GHT newlib/syscall glue |
| `lv2.ld` | `runtime/lv2/crt/lv2.ld` | native PS3DK linker script; keeps `.opd ALIGN(4)` |
| `lv2-crtn.o` | PSL1GHT, where still used by startfile specs | not yet replaced |

Important runtime facts:

- `crt0.S` deliberately loads the TOC from compact descriptor offset `+4`.
- `lv2.ld` deliberately aligns `.opd` to 4 bytes.
- `lv2-sprx.S` owns the current 64-byte process parameter block.
- `build-runtime-lv2.sh` must be rerun after rebuilding PSL1GHT because PSL1GHT
  can overwrite startfiles in the GCC search path.

### 5. `tools/sprx-linker`

Source: `tools/sprx-linker/`.

Current responsibilities:

- verify `.sys_proc_prx_param` by checking the Lv-2 magic word;
- accept both legacy 40-byte and current 64-byte process-param sections for
  compatibility;
- fill each `.lib.stub` record's `imports` count from the FNID table;
- preserve a compatibility `.opd` pack step only for legacy 24-byte ELFv1 input;
- skip native compact `.opd` sections.

`sprx-linker` is no longer the mechanism that makes native PS3DK `.opd` output
work. Compact `.opd` is produced before this tool runs. The tool remains in the
pipeline because `.lib.stub` import counts are still a post-link fixup, and
because accepting old 24-byte inputs is useful while the wider runtime migration
is ongoing.

### 6. SELF / fake-SELF wrappers

Source: PSL1GHT/ps3dev-supplied tools such as `make_self` and `fself`.

Responsibilities:

- wrap the linked/fixed ELF into a `.self` or `.fake.self` container;
- produce artifacts suitable for hardware or RPCS3 testing.

These tools are not the source of the CellOS ABI rules described here; they are
packaging tools after the ELF has already been shaped by the compiler, linker,
CRT, and post-link fixups.

### 7. SDK headers and libraries

Source: `sdk/`.

Responsibilities:

- expose public headers such as `cell/gcm.h`, `cell/pad.h`,
  `cell/sysutil.h`, `sys/process.h`, and `sys/lv2_types.h`;
- distinguish native 64-bit public pointers from explicit 32-bit EA fields;
- provide `lv2_ea32_t` and conversion helpers;
- provide `lv2_fn_to_callback_ea`, now a bare cast for native compact `.opd`;
- bridge some PSL1GHT-style APIs to Cell SDK-style/NID-backed APIs while the SDK
  surface is being filled out.

Important distinction:

- Public reference-style structs should use native pointers when the public API
  expects native pointers.
- Kernel-facing payload structs may need 32-bit EA fields. Those should be
  represented explicitly, preferably with `lv2_ea32_t`, rather than by relying
  indefinitely on `void * __attribute__((mode(SI)))`.

Compatibility wrapper libraries such as `sdk/libgcm_sys_legacy/` are still part
of the tree. They are compatibility layers, not proof that the final native SDK
surface is complete.

### 8. NID stub archives — `tools/nidgen`

Source: `tools/nidgen/`.

Responsibilities:

- read library/function NID databases;
- generate per-library stub archives;
- emit `.sceStub.text`, `.rodata.sceFNID`, `.rodata.sceVStub`, and
  `.data.sceFStub` fragments;
- provide the import metadata consumed by the Lv-2 loader.

At runtime, the loader walks `.lib.stub`, resolves each FNID, and writes the
resolved function descriptor EA into `.data.sceFStub`. The generated stub then
jumps through the compact descriptor contract.

### 9. ABI verification — `tools/abi-verify`

Source: `tools/abi-verify/`.

Responsibilities:

- parse ELF objects, archives, and linked binaries;
- report ELF identity, section layout, `.opd` descriptor shape, and process
  parameter contents;
- reject obvious ABI drift such as 24-byte `.opd` leakage in native output or
  `R_PPC64_ADDR64` in places that must be 32-bit EA fields;
- compare representative outputs against fixtures.

This should be the first tool run after changing GCC, binutils, CRT, linker
script, SDK struct layout, or post-link logic.

---

## Data flow: source to runnable artifact

```text
hello.c / hello.cpp
  |
  | 1. GCC
  |    - compiles to PPC64 big-endian code
  |    - emits compact .opd descriptors
  |    - emits compact indirect-call sequences
  v
hello.o
  |
  | 2. ld + lv2.ld + CRT + archives
  |    - lays out Lv-2 sections and program headers
  |    - resolves relocations
  |    - keeps .opd compact/aligned
  |    - includes .sys_proc_prx_param
  v
hello.elf
  |
  | 3. sprx-linker
  |    - verifies process-param magic
  |    - fills .lib.stub import counts
  |    - skips compact .opd, or packs only legacy 24-byte .opd input
  v
hello.elf
  |
  | 4. make_self / fself
  v
hello.self / hello.fake.self
```

At runtime, the Lv-2 loader:

1. reads the ELF/program header identity;
2. locates the process parameter block;
3. maps load segments;
4. resolves NID imports through `.lib.stub` / `.rodata.sceFNID`;
5. writes resolved descriptor EAs into `.data.sceFStub`;
6. enters the CRT start path, which initializes the process and calls `main`.

---

## Current ABI conformance status

| Invariant | Status | Delivered by |
|---|---|---|
| ELF64, big-endian, PPC64 | implemented | GCC + binutils |
| `EI_OSABI = 0x66` | implemented / required | binutils/toolchain identity path |
| `e_flags = 0x00000000` | normative current value | ABI spec; older `0x01000000` notes are stale |
| 64-byte `.sys_proc_prx_param` | implemented | `runtime/lv2/crt/lv2-sprx.S` |
| `.sys_proc_prx_param` magic/version/layout | implemented | `lv2-sprx.S` + `lv2.ld` |
| `.opd` entry size = 8 bytes | implemented | GCC compact `.opd` path + linker script |
| compact indirect-call sequence | implemented | GCC rs6000 backend patches |
| callback EA conversion without `+16` | implemented | `sdk/include/sys/lv2_types.h` |
| ELF64 + ILP32 hybrid (Pmode = SImode default) | implemented (2026-05-07) | GCC patches 0005 + 0021; multilib `-mlp64` opt-in |
| 4-byte TOC slots (`R_PPC64_ADDR32`) | implemented (2026-05-07) | GCC patch 0021 (`output_toc` Pmode gate); libgloss force-rebuild defense in `build-ppu-toolchain.sh` |
| Framed SPRX trampoline (LR @ caller_sp+16, TOC @ trampoline_sp+24, 64-byte frame) | implemented (2026-05-07) | `tools/nidgen/src/stubgen.rs` |
| Single `.lib.stub` per imported library | implemented (2026-05-07) | nidgen alias-fold absorbs PSL1GHT-style names; `liblv2.a/sprx.o` retired |
| PSL1GHT-name compatibility via aliases | implemented (2026-05-07) | nidgen `aliases:` field per export |
| legacy 24-byte `.opd` compatibility fallback | retained | `tools/sprx-linker` |
| full PSL1GHT-free CRT | not complete | `lv2-crt0.o` still merges PSL1GHT `crt1.o` |
| full PSL1GHT-free SDK surface | not complete | legacy compatibility wrappers still exist |
| complete NID/API coverage | ongoing | `tools/nidgen` + `sdk/` |

---

## Where to change things

- ELF identity or relocation/link behavior: binutils patches.
- Function descriptor layout or indirect calls: GCC rs6000 backend patches.
- Process parameter block, start path, or linker layout: `runtime/lv2/crt/`.
- Imported function metadata or stub generation: `tools/nidgen/` and
  `tools/sprx-linker/`.
- Public API structs and pointer/EA boundaries: `sdk/include/` and
  compatibility libraries under `sdk/`.
- Structural regression checks: `tools/abi-verify/`.

Always run `abi-verify check` and at least one RPCS3/hardware validation sample after
changing any ABI-facing layer.
