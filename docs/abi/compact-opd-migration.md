# Native compact `.opd` emission

This document records the current state of the compact function-descriptor
migration for the PPU `powerpc64-ps3-elf` toolchain.

The migration is no longer a proposal: PS3DK now targets native 8-byte CellOS
Lv-2 function descriptors directly. The old PSL1GHT-style compatibility trick
of emitting standard 24-byte PPC64 ELFv1 descriptors and storing a compact
descriptor copy in the environment slot is no longer the normal ABI path.

For the normative byte-level contract, see `docs/abi/cellos-lv2-abi-spec.md`.
For section layouts, see `docs/abi/section-reference.md`. For which component
emits or consumes each piece, see `docs/abi/toolchain-architecture.md`.

---

## Current state

A conforming PPU object or linked binary uses an 8-byte `.opd` descriptor:

```text
offset  size  contents
------  ----  -----------------------------------------------
0x00    4     function entry EA, 32-bit
0x04    4     module TOC base EA, 32-bit
```

The descriptor stride is 8 bytes and the `.opd` section alignment is 4 bytes.
A C function pointer is still a native 64-bit PPU pointer, but the address it
contains points at this compact descriptor instead of a 24-byte PPC64 ELFv1
descriptor.

The practical results are:

- `lv2_fn_to_callback_ea(fn)` is a bare cast to the descriptor EA.
- The old `+16` callback offset is retired for native PS3DK output.
- `__get_opd32` is not part of the native path.
- The linker script keeps `.opd` at `ALIGN(4)` so compact descriptors are not
  promoted back into the legacy 24-byte-edit path.
- `tools/sprx-linker` leaves compact `.opd` sections alone.

---

## Why this needed coordinated compiler/linker/runtime work

Stock PPC64 ELFv1 code expects a 24-byte function descriptor:

```text
offset  size  contents
------  ----  ----------------
0x00    8     entry address
0x08    8     TOC address
0x10    8     environment pointer
```

The corresponding indirect-call sequence reads across all three slots:

```asm
ld    r10, 0(r9)
ld    r11, 16(r9)
mtctr r10
ld    r2,  8(r9)
bctrl
```

Shrinking `.opd` at link or post-link time without changing the compiler would
make those loads read into adjacent descriptors. That is why the compact format
requires both:

1. native 8-byte descriptor emission; and
2. an indirect-call sequence that reads two 32-bit words from the compact
   descriptor.

The compact call path is:

```asm
std   r2, 40(r1)
lwz   r0, 0(r9)
mtctr r0
lwz   r2, 4(r9)
bctrl
ld    r2, 40(r1)
```

`lwz` is intentional: Lv-2 user effective addresses fit in 32 bits, while the
PPU ABI still uses 64-bit registers and native 64-bit C pointers.

---

## Implemented pieces

### GCC

The PPU GCC target emits compact descriptors and the matching compact
indirect-call sequence for `powerpc64-ps3-elf`.

The important compiler-side ABI properties are:

- `.opd` descriptors are emitted with an 8-byte stride.
- descriptor fields are 32-bit effective addresses;
- indirect calls load entry and TOC with `lwz`;
- public C/C++ function pointers remain native 64-bit pointers to descriptors.

### Binutils / linker

The linker is responsible for normal section layout, relocation resolution, and
the final ELF identity bytes. For compact `.opd`, the relevant properties are:

- `.opd` must remain 4-byte aligned in final output;
- descriptor entry fields resolve to 32-bit code EAs;
- descriptor TOC fields resolve to the module TOC base EA;
- `R_PPC64_ADDR64` must not leak into `.opd` in conforming output.

Some inputs may encode the TOC field with a direct `.TOC.` relocation, while
the compiler/binutils path may use the CellOS-specific marker described in the
ABI spec. The final linked descriptor bytes are the important contract: two
32-bit words, `[entry_ea, toc_ea]`.

### Runtime start files

`runtime/lv2/crt/crt0.S` provides a compact `__start` descriptor and explicitly
loads the TOC from descriptor offset `+4`, not the PPC64 ELFv1 `+8` slot.

`runtime/lv2/crt/lv2.ld` keeps `.opd` at `ALIGN(4)`. This matters because the
post-link tool uses section alignment to distinguish native compact descriptors
from legacy 24-byte ELFv1 descriptors.

`build-runtime-lv2.sh` currently installs native `lv2-sprx.o`, native
`lv2-crti.o`, native `crt0.o` merged into `lv2-crt0.o`, and the native
`lv2.ld`. The current `lv2-crt0.o` still reuses PSL1GHT's `crt1.o` for the
unchanged newlib/syscall glue, so the CRT is not yet completely PSL1GHT-free.

### `tools/sprx-linker`

`sprx-linker` is still part of the build, but its role is narrower than it used
to be.

Current responsibilities:

- verify that `.sys_proc_prx_param` exists and has the Lv-2 magic word;
- fill per-library import counts in `.lib.stub`;
- leave native compact `.opd` sections untouched;
- preserve a compatibility fallback for legacy 24-byte ELFv1 `.opd` inputs.

The legacy fallback is gated on `.opd` section alignment. Compact descriptors
are 4-byte aligned; legacy ELFv1 descriptors are 8-byte aligned. Size
divisibility alone is not reliable because an 8-byte compact section with a
multiple of three entries is also divisible by 24.

---

## Retired transitional form

The retired PSL1GHT compatibility form was:

```text
offset  size  contents
------  ----  ---------------------------------------------------------
0x00    8     PPC64 ELFv1 entry address
0x08    8     PPC64 ELFv1 TOC address
0x10    8     packed compact descriptor: { entry_lo32, toc_lo32 }
```

That form existed so Lv-2 callbacks could pass `descriptor + 16` to the kernel
while GCC still generated normal PPC64 ELFv1 descriptors. Native PS3DK output no
longer relies on that layout.

The post-link packer is retained only as a compatibility path for any legacy
24-byte input that still reaches `sprx-linker`.

---

## Validation checklist

Compact `.opd` output should be considered healthy when:

1. `.opd` has 8-byte descriptor stride and 4-byte alignment.
2. no `.opd` relocation resolves through `R_PPC64_ADDR64`;
3. indirect-call disassembly uses the compact `lwz` load sequence;
4. `lv2_fn_to_callback_ea(fn)` returns the descriptor EA directly;
5. `sprx-linker` does not rewrite compact `.opd` sections;
6. `abi-verify check` passes for representative samples;
7. callback-using samples, such as GCM/vblank paths, run under RPCS3 or hardware.

---

## Still not solved by this migration

Compact `.opd` was the largest ABI wart, but it is not the whole SDK/runtime
migration.

Still separate from this document:

- replacing the remaining PSL1GHT `crt1.o` dependency;
- removing all remaining `mode(SI)`-style pointer declarations from legacy
  interfaces in favor of explicit `lv2_ea32_t` kernel-side fields;
- replacing or retiring PSL1GHT compatibility wrapper libraries where native
  Cell SDK-style APIs are complete;
- expanding NID stub coverage and implementation verification.

So the compact-descriptor ABI path is implemented, but the broader goal of a
fully PSL1GHT-free runtime and SDK is still ongoing.
