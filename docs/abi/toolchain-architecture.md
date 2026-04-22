# CellOS Lv-2 toolchain architecture

Companion to `docs/abi/cellos-lv2-abi-spec.md` (the normative binary
contract). That document says *what* a conformant ELF must look like.
This document explains *how* the pieces of the toolchain cooperate
to produce one.

The Lv-2 ABI is not a standard PowerPC ELFv1 target. It is a
PPC64 big-endian ELF64 with:

- A distinct OS/ABI identifier byte (`EI_OSABI = 0x66`).
- A compact 8-byte function-descriptor format in `.opd`, not the
  standard 24-byte three-doubleword form.
- A 64-byte `.sys_proc_prx_param` process metadata section with a
  specific magic + layout the Lv-2 loader reads.
- A 64-bit ABI where pointers are 64-bit but userland effective
  addresses fit in 32 bits; a handful of ABI-fixed fields carry
  explicit 32-bit EA values.
- A module-per-TOC model: every `.opd` entry carries the callee
  module's TOC base EA in its trailing 4 bytes, enabling
  cross-module indirect calls without stubs.

Producing such an ELF requires coordinated behaviour across every
layer of the toolchain. This doc maps each layer's role so a future
contributor can reason about where a given conformance property
lives and who to modify to change it.

---

## Layer-by-layer map

### 1. GCC (PPU target `powerpc64-ps3-elf`, 12.4.0)

Source: `src/upstream/gcc/gcc/config/rs6000/` (patched via `patches/ppu/gcc-12.x/`).

Responsibilities:

- Emits PPU machine code (`.text`) and function-descriptor entries
  (`.opd`) for every function it compiles.
- Emits the indirect-call instruction sequence that reads a function
  descriptor at runtime.
- Emits process parameter section relocations (`.sys_proc_prx_param`)
  via the user's `SYS_PROCESS_PARAM()` macro in `<sys/process.h>`.
- Provides the set of predefined macros (e.g. `POWERPC_CELL64LV2`) that
  target-specific headers use to gate Lv-2-specific behaviour.

Key files in our patched GCC:

| File | Role |
|---|---|
| `gcc/config/rs6000/cell64lv2.h` | Lv-2 target header — specs for link-start/libs, OS/ABI defines, `POWERPC_CELL64LV2` macro. Ships in `patches/ppu/gcc-12.x/0002-*.patch`. |
| `gcc/config/rs6000/rs6000.cc` | rs6000 backend — `rs6000_elf_declare_function_name` emits per-function `.opd` entries; `rs6000_call_aix` expands indirect-call RTL; `rs6000_indirect_call_template_1` formats the final instruction template. |
| `gcc/config/rs6000/ppc-asm.h` | Macros (`FUNC_START` etc.) that libgcc and hand-written asm use to emit function descriptors. |

Open work (the `compact-opd migration`, tracked in internal
planning): the backend currently emits 24-byte ELFv1 descriptors and
a 3-read 64-bit indirect-call sequence. The target end state is an
8-byte descriptor and a 2-read 32-bit `lwz` sequence matching §2 of
the spec. Until that lands, the post-link step (see §4) packs a
compat 8-byte block into the env slot of the 24-byte descriptor so
loader-visible callback paths still work.

---

### 2. Binutils (2.42)

Source: `src/upstream/binutils-gdb/` (patched via `patches/ppu/binutils-2.42/`).

Responsibilities:

- Assembler (`as`) converts the compiler's textual asm into REL
  objects. Emits the specific relocations the linker later resolves
  (`R_PPC64_ADDR32`, `R_PPC64_ADDR64`, `R_PPC64_REL24`, etc.).
- Linker (`ld`) merges REL objects and archives into a loadable ELF.
  Applies relocations, lays out sections, writes the output.
- Our patch (`patches/ppu/binutils-2.42/0001-elf64-ppc-tag-cellos-lv2-headers.patch`)
  adds a `ppc64_elf_final_write_processing` hook: when the output
  contains a `.sys_proc_prx_param` section — the marker that this is
  Lv-2 userland — the hook writes `EI_OSABI = 0x66` and
  `e_flags = 0x01000000` into the ELF header. Standard ppc64 targets
  (Linux, FreeBSD, bare-metal) never emit that section and are
  unaffected.

The rest of the ppc64 backend (`bfd/elf64-ppc.c`) handles `.opd`
entry optimisation, TOC section manipulation, and the
rela→resolved-bytes pipeline unchanged.

Why binutils is (deliberately) minimal: the Lv-2 ABI's divergence
from upstream ppc64-elf is mostly at the compiler-emission layer
(GCC) and at the process-parameter / CRT layer (our `runtime/lv2/`),
not at link-time. The linker just resolves what GCC emits.

---

### 3. Newlib (4.4.0.20231231)

Source: `src/upstream/newlib/` (patched via `patches/ppu/newlib-4.x/`).

Responsibilities:

- Provides the C standard library (`libc.a`, `libm.a`).
- Provides `libgloss` / `libsysbase` — the low-level syscall layer
  that routes `read`, `write`, `malloc`, `open`, etc. through
  Lv-2 syscalls. This is where `printf → sys_tty_write` happens.
- Provides the reentrant-locks layer (`libsysbase` + `reent.h`) that
  keeps libc thread-safe under Lv-2's thread model.

Our patches:

| File | Role |
|---|---|
| `0001-newlib-add-ps3-lv2-target-config.patch` | Adds the `powerpc64-ps3-elf` host case to `newlib/configure.host`. |
| `0002-libgloss-add-libsysbase-subdir.patch` | Wires the Lv-2 libsysbase build into libgloss. |
| `0003-libsysbase-ps3-syscall-wrappers.patch` | PS3-specific syscall wrapper stubs (sbrk, fd table, etc.). |
| `0004-newlib-libc-sys-lv2-reent-locks.patch` | Lv-2 reentrant-locks glue. |
| Misc (`0005`–`0010`) | Math library overflow fixes, autoconf compat, dirent / getentropy stubs. |

Newlib doesn't emit any Lv-2-specific ELF markers itself — those come
from the CRT and the `SYS_PROCESS_PARAM()` macro in sys/process.h.
Newlib just happens to run inside that environment.

---

### 4. CRT + runtime layer

Source: `runtime/lv2/crt/` (our native) + `src/ps3dev/PSL1GHT/ppu/crt/` (PSL1GHT-supplied).

Responsibilities:

- Provides the startfiles linked in by default: `lv2-crti.o`,
  `lv2-crt0.o`, `lv2-sprx.o`, `lv2-crtn.o`. These run before and
  after `main()`.
- Emits the `.sys_proc_prx_param` section with the Lv-2 magic,
  version, SDK version, and the library entry/stub range markers
  the loader needs.
- Sets up the initial stack, calls constructors, runs `main`, runs
  destructors, calls `sys_process_exit`.

Currently hybrid:

| Startfile | Source | Owner | Notes |
|---|---|---|---|
| `lv2-crti.o` | PSL1GHT | Upstream | `.init` / `.fini` prologue. |
| `lv2-crt0.o` | PSL1GHT | Upstream | Program entry `_start`, calls `_initialize`. |
| `lv2-sprx.o` | `runtime/lv2/crt/lv2-sprx.S` | **Us** | Emits the spec-correct 64-byte `.sys_proc_prx_param` plus `.lib.ent.top` / `.lib.stub.top` prefix headers. Overrides PSL1GHT's version via `-B $(PS3DK)/ppu/lib/` in sample Makefiles. |
| `lv2-crtn.o` | PSL1GHT | Upstream | `.init` / `.fini` epilogue. |
| `lv2.ld` | PSL1GHT | Upstream | Linker script — section ordering, phdr layout. |

The `.sys_proc_prx_param` swap (Lv-2-identity commit on
`feature/lv2-abi-native` branch) is the one place where we
currently deviate from stock PSL1GHT CRT. Progressive replacement of
the rest is open work (tracked in internal planning).

---

### 5. Post-link fixups — `sprx-linker`

Source: `tools/sprx-linker/`.

Responsibilities (operates on an already-linked ELF):

- Verifies the `.sys_proc_prx_param` section is present with the
  expected magic at offset 4 (accepts either the 40-byte legacy or
  the 64-byte spec-correct size).
- Walks `.lib.stub` stub table + `.rodata.sceFNID` table. For each
  stub, computes the number of FNID entries it covers and writes
  that count back into the stub's `imports` field.
- Walks `.opd`. If entries are 24 bytes (current ELFv1 output), packs
  a compact 8-byte `(entry_lo32, toc_lo32)` block into the env slot
  at offset +16 of each entry, so the Lv-2 loader's callback
  dereference finds a valid compact descriptor there when given a
  callback address computed via `lv2_fn_to_callback_ea(fn)`
  (`fn + 16`). If entries are already 8 bytes, leaves them alone.

This is a transitional tool — when the compact-opd migration lands,
the `.opd` packing step becomes a no-op for all inputs and the tool
can be retired.

---

### 6. SELF / fake-SELF wrappers

Source: PSL1GHT-supplied (`make_self`, `fself`).

Responsibilities (wrap the post-link, post-sprxlinker ELF into the
container format the Lv-2 loader / RPCS3 actually reads):

- `make_self` — produces the CEX-signed `.self` variant.
- `fself` — produces the unsigned fake `.fake.self` variant (used for
  RPCS3 testing).

These tools don't inspect `.sys_proc_prx_param` contents and work
unchanged with our spec-correct ELFs. No fork needed.

---

### 7. SDK layer (our header + library surface)

Source: `sdk/`.

Responsibilities:

- Provides the public API headers user code includes: `cell/gcm.h`,
  `cell/sysutil.h`, `cell/pad.h`, `sys/process.h`, `sys/lv2_types.h`,
  etc.
- Provides the typed bridges between our spec-conformant public API
  (e.g. `CellGcmConfig` with native 64-bit pointers) and the Lv-2
  kernel-interface layouts (e.g. `CellGcmConfigRaw` with
  `lv2_ea32_t`).
- Provides compatibility wrappers (`sdk/libgcm_sys_legacy/`) that
  implement the PSL1GHT-flavoured `gcm*` names still emitted by
  `<cell/gcm.h>`'s static-inline forwarders. These call into the
  nidgen-generated `cellGcm*` NID stubs for the actual Lv-2 syscalls.

Key types defined here:

- `lv2_ea32_t` (`sdk/include/sys/lv2_types.h`) — typedef for
  32-bit effective address fields in Lv-2 ABI slots. `lv2_ea32_pack`
  / `lv2_ea32_expand` helpers convert in and out, with debug-mode
  upper-bits assertions.
- `lv2_fn_to_callback_ea` (same header) — converts a C function
  pointer into the 32-bit EA the Lv-2 kernel callback-registration
  syscalls expect. Currently returns `fn + 16` (the compat offset
  sprx-linker packs into a 24-byte descriptor's env slot). In the
  compact-opd target state the body becomes a bare cast.

---

### 8. NID stub archives — `nidgen`

Source: `tools/nidgen/`.

Responsibilities (build-time):

- Reads NID (named-identifier hash) databases describing every
  Lv-2-kernel-exported function (`cellGcmInit`, `cellPadRead`, etc.),
  the library they live in, and the library's own NID.
- Emits per-library stub archives containing one small assembly stub
  per exported function. Each stub:
  - Lives in `.sceStub.text` with a `.sceStub.text.<funcname>` alias.
  - Has a matching entry in `.rodata.sceFNID` (the FNID hash, 4 bytes).
  - Has a matching entry in `.data.sceFStub` (a relocation slot the
    loader patches at load time with the real function address).
  - Has a matching entry in `.rodata.sceVStub` for library metadata.
- Produces `libfoo_stub.a` per library. Linked into user programs as
  `-lfoo_stub`.

At load time, the Lv-2 loader walks the linked binary's `.lib.stub`
table, for each stub looks up the function by (library NID, function
NID), writes the resolved address into `.data.sceFStub`, and the
stub's code reads that address at call time.

Coverage status: `tools/nidgen/coverage-report` outputs a per-library
completeness report.

---

### 9. Conformance verification — `abi-verify`

Source: `tools/abi-verify/`.

Responsibilities:

- Parses ELF objects / archives / linked binaries.
- Emits per-file JSON manifests (ELF header, section summary, `.opd`
  descriptor shape, `.sys_proc_prx_param` contents + relocations).
- Runs invariant checks (ELFCLASS64, big-endian, `EM_PPC64`,
  `EI_OSABI = 0x66`, `e_flags = 0x01000000`, `.opd` entry-size
  expectations, `.sys_proc_prx_param` size + magic).
- Diffs sample outputs against reference fixtures in
  `tests/abi/fixtures/`.

Used in two ways:

1. As a CI gate (not yet wired into CI): every sample build can be
   followed by `abi-verify check` to catch regressions.
2. As an interactive probe during ABI work — dump manifests of our
   output vs reference fixtures to find drift.

---

## Data flow: `.c` → `.fake.self`

How a single source file becomes a runnable binary:

```
hello.c
  │
  │  1. GCC compiles → hello.o (REL object, .text + .opd (24-byte) + .toc + relocations)
  │     — emits .opd entries for every function defined here
  │     — emits indirect-call sequences reading 8-byte-stride descriptors (target state)
  │     — emits .sys_proc_prx_param relocations from SYS_PROCESS_PARAM() macro
  ▼
hello.o
  │
  │  2. ld links hello.o + libsysbase.a + liblv2.a + lv2-{crti,crt0,sprx,crtn}.o → hello.elf
  │     — resolves all relocations
  │     — lays out sections per lv2.ld linker script
  │     — hook: elf_backend_final_write_processing sees .sys_proc_prx_param present,
  │       writes EI_OSABI=0x66 and e_flags=0x01000000 into the ELF header
  ▼
hello.elf (post-binutils)
  │
  │  3. strip → hello.elf (stripped, for SELF build)
  │
  │  4. sprx-linker hello.elf:
  │     — verifies .sys_proc_prx_param magic
  │     — fills .lib.stub import counts
  │     — (current state) packs compact 8-byte descriptor at offset +16
  │       of each 24-byte .opd entry
  │     — (compact-opd target state) no-op: .opd entries are already 8 bytes
  ▼
hello.elf (post-sprxlinker, ready for SELF wrap)
  │
  │  5a. make_self hello.elf hello.self            (CEX variant)
  │  5b. fself hello.elf hello.fake.self           (RPCS3 variant)
  ▼
hello.self / hello.fake.self
```

At runtime on RPCS3 or hardware the Lv-2 loader:

1. Validates ELF identity (`EI_OSABI`, `e_flags`, `e_machine`).
2. Reads `.sys_proc_prx_param`, pulls out the SDK version, process
   enable flags, and library entry/stub range markers.
3. Maps LOAD segments into memory.
4. Walks `.lib.stub`, for each entry resolves the imported
   function via NID, writes the resolved address into
   `.data.sceFStub`.
5. Jumps to `_start` (the CRT entry), which sets up stack/TLS, runs
   init arrays, calls `main`, runs fini arrays, calls
   `sys_process_exit`.

---

## Current ABI conformance status

As of the current HEAD:

| Invariant (per spec) | Status | Delivered by |
|---|---|---|
| ELF64, big-endian, PPC64 | ✓ | GCC + binutils (stock) |
| `EI_OSABI = 0x66` | ✓ | binutils patch `0001-elf64-ppc-tag-cellos-lv2-headers` |
| `e_flags = 0x01000000` | ✓ | binutils patch `0001-elf64-ppc-tag-cellos-lv2-headers` |
| `.sys_proc_prx_param` size 0x40, magic 0x1b434cec, version 4 | ✓ | `runtime/lv2/crt/lv2-sprx.S` (our native startfile) |
| `.sys_proc_prx_param` field layout | ✓ | same |
| `.opd` entry size = 8 bytes | ✗ (target state) | requires compact-opd migration (GCC change) |
| `.opd` reloc pattern `ADDR32 + TLSGD` | ✗ (target state) | same |
| 64-bit native pointers in public API | ✓ | `sdk/include/cell/*.h` |
| `lv2_ea32_t` explicit in EA slots | ✓ (available, in-use in widener) | `sdk/include/sys/lv2_types.h` |
| No `mode(SI)` in public API | ✓ | Our headers never declare it. PSL1GHT headers do; we refactor away from them progressively. |
| Relocation class contract | ✓ | binutils + GCC (stock) |
| Sample runs under RPCS3 with expected output | ✓ | Sample matrix passes. |

The remaining conformance work is the compact-opd migration: GCC
changes to emit 8-byte `.opd` entries and a 2-read `lwz` indirect-call
sequence. Once that lands, the `lv2_fn_to_callback_ea` helper's `+16`
offset disappears, `sprx-linker`'s `.opd` packing step is retired,
and PSL1GHT's `__get_opd32` macro stops being reachable from our
tracked code.

---

## Where to start when changing a conformance property

- Something about the ELF header or a segment flag → binutils patch
  or `runtime/lv2/crt/` linker script.
- Something about the `.sys_proc_prx_param` layout or startfile
  behaviour → `runtime/lv2/crt/lv2-sprx.S` + spec section 3.
- Something about `.opd` content or indirect-call instruction
  sequence → GCC rs6000 backend + spec section 2.
- Something about a public API struct or helper → `sdk/include/`
  and `sdk/libgcm_sys_legacy/`.
- Something about a missing Lv-2 syscall symbol → `tools/nidgen/`
  database + stub archive build.
- Something about a post-link structural check → extend
  `tools/abi-verify/src/lib.rs`.

Always run `abi-verify check` on the changed sample output before
concluding any ABI work is done.
