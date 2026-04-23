# CellOS Lv-2 ELF section reference

Byte-level reference for every Lv-2-specific ELF section and table
this toolchain emits or consumes. Companion to
`docs/abi/cellos-lv2-abi-spec.md` (normative spec, terse) and
`docs/abi/toolchain-architecture.md` (who emits what).

Two things this document covers that the spec doesn't:

1. The **current** on-disk layouts (8-byte ELFv1 `.opd` plus a
   compat packing trick), not just the target-state layouts.
2. The full relationship between the relocation-level contract, the
   instruction-level contract, and the loader-level contract for
   each section — so someone changing one of them has enough
   context to reason about the blast radius.

All offsets are from the section base unless stated otherwise. All
multi-byte values are big-endian (`ELFDATA2MSB`).

---

## 1. `.opd` — function descriptors

`.opd` holds one fixed-size entry per function whose address is
taken (or which is exported). The Lv-2 ABI uses an 8-byte compact
form:

```
offset  size  contents                               reloc
------  ----  ----------------------------------     ------------------
0x00    4     Function entry-point EA (32-bit)       R_PPC64_ADDR32 .funcname
0x04    4     Module TOC base EA (32-bit)            R_PPC64_ADDR32 .TOC.
                                                     (or R_PPC64_TLSGD marker;
                                                      both resolve to the same
                                                      value — see §1.3)
```

Section attributes:

- `sh_type = SHT_PROGBITS`
- `sh_flags = SHF_WRITE | SHF_ALLOC` (`WA`)
- `sh_addralign = 4`
- `sh_entsize = 0` (variable-format convention; descriptors are a
  fixed 8-byte stride but the header reports 0)

### 1.1 Interpretation

Every function descriptor is exactly 8 bytes. A C function pointer
is the address of the descriptor, and is 8 bytes wide (a native
PPU64 pointer). Reading through the descriptor:

- The first 4 bytes are the code entry-point EA in the module's
  text segment.
- The next 4 bytes are the module's TOC base EA. Every descriptor
  in a given module has the same value here — it is a
  module-level constant, not a per-function value.

The module TOC is the memory region (by convention a contiguous
section aliased as `.TOC.`) that holds the module's addressing
tables: symbol-address array, globals, module-private data
pointers. Every function in the module uses this TOC via `r2`.

### 1.2 Indirect-call sequence

To call through a compact descriptor (pointer in `r9`):

```asm
std   r2, 40(r1)     ; save caller TOC (standard ABI stack slot)
lwz   r0, 0(r9)      ; r0 = entry EA (zero-extends word to full reg)
mtctr r0
lwz   r2, 4(r9)      ; r2 = callee's module TOC base
bctrl                ; call
ld    r2, 40(r1)     ; restore caller TOC (emitted in function epilogue
                     ; or at the next call site that needs r2)
```

Four instructions on the call path plus two for TOC save/restore.
`lwz` is the 32-bit load-zero-extend instruction — it reads 4 bytes
and zeroes the upper 32 bits of the 64-bit register, which is
correct for Lv-2 userland EAs that fit in 32 bits.

Note that TOC switching happens automatically on every indirect
call: the caller's TOC is saved to the standard stack slot at
`40(r1)`, and the callee's TOC is loaded from the descriptor.
Cross-module indirect calls therefore work without any stub or
trampoline — the descriptor itself carries the right TOC.

### 1.3 TOC-field encoding alternatives

The 32-bit TOC field at `.opd+4` can be emitted two ways. Both
resolve to the same value at link time:

**(a)** `R_PPC64_ADDR32 .TOC. + 0` — ordinary 32-bit address
relocation against the `.TOC.` symbol the linker provides at the
TOC base. Straightforward and supported by stock binutils.

**(b)** `R_PPC64_TLSGD *ABS* + 0` — a TLS-general-dynamic marker
with null symbol and zero addend, repurposed by some linkers as a
"write module TOC base here" directive. Appears in some
reference-SDK-produced REL object files. Functionally identical to
(a) after link.

This toolchain uses (a) (plain `.long .TOC.` in assembly) because
it requires no binutils patch and produces byte-identical output.

### 1.4 Why two reads, not three

The PowerPC ELFv1 standard defined a 24-byte function descriptor:
`[entry_addr_64, toc_addr_64, env_64]`. GCC's default ppc64 indirect-call
sequence reads all three doublewords (`ld r10, 0(r9); ld r11, 16(r9);
ld r2, 8(r9)`).

Lv-2 dropped the `env` doubleword — the upper 8 bytes of a 24-byte
entry were unused for non-nested-function callees. Shrinking to 8
bytes (2×`.long` instead of 3×`.quad`) halved `.opd` size and
adjusted the call sequence to `lwz` reads of the low 32 bits.

### 1.5 Native 8-byte `.opd` emission (achieved)

The current toolchain emits native 8-byte `.opd` entries:

```
offset  size  contents                               reloc
------  ----  ----------------------------------     ------------------
0x00    4     Function entry-point EA (32-bit)       R_PPC64_ADDR32 .funcname
0x04    4     Module TOC base EA (32-bit)            R_PPC64_TLSGD *ABS* +0
```

The `R_PPC64_TLSGD *ABS*` reloc at offset +4 is resolved by the linker
to write the module's TOC base EA into that slot. This is a repurposed
TLS reloc used as a link-time fill directive — each descriptor in a
given module carries the same TOC value (the callee's module TOC base
for cross-module indirect calls).

The 8-byte layout matches what the reference SDK produces and what the
Lv-2 ABI spec (§2) normatively requires. The `.opd` section size is
`8 × n_functions`, not `24 × n_functions`.

### 1.6 Transitional form — retired

A transitional output format, produced by GCC 12's stock ppc64 backend
plus `tools/sprx-linker` post-link pass:

```
offset  size  contents                        reloc
------  ----  ------------------------------  --------------------
0x00    8     entry_addr (64-bit, low 32 = actual EA, upper 32 = 0)
0x08    8     toc_addr (64-bit, low 32 = TOC base EA, upper 32 = 0)
0x10    8     compat compact descriptor: {entry_lo32, toc_lo32}
              (packed post-link by sprx-linker; originally an `env`
              slot written as .quad 0 by the compiler)
```

This form has been retired. GCC now emits native 8-byte `.opd` entries
directly; `lv2_fn_to_callback_ea`'s `+16` offset has collapsed to a bare
cast. The packing step in sprx-linker is no longer needed.

### 1.7 Instruction sequence for native 8-byte `.opd`

The indirect-call sequence the current toolchain emits:

```asm
std   r2, 40(r1)     ; save caller TOC (standard ABI stack slot)
lwz   r0, 0(r9)      ; r0 = entry EA (zero-extends word to full reg)
mtctr r0
lwz   r2, 4(r9)      ; r2 = callee's module TOC base
bctrl                ; call
ld    r2, 40(r1)     ; restore caller TOC (emitted in function epilogue
                     ; or at the next call site that needs r2)
```

Four instructions on the call path plus two for TOC save/restore.
`lwz` is the 32-bit load-zero-extend instruction — it reads 4 bytes
and zeroes the upper 32 bits of the 64-bit register, which is
correct for Lv-2 userland EAs that fit in 32 bits.

Note that TOC switching happens automatically on every indirect
call: the caller's TOC is saved to the standard stack slot at
`40(r1)`, and the callee's TOC is loaded from the descriptor.
Cross-module indirect calls therefore work without any stub or
trampoline — the descriptor itself carries the right TOC.

For completeness, the 3-read sequence that was used with the
transitional (retired) toolchain:

```asm
std   r2, 40(r1)     ; save caller TOC
ld    r10, 0(r9)     ; r10 = entry_addr (8-byte load)
ld    r11, 16(r9)    ; r11 = env (8-byte load; unused for non-nested)
mtctr r10
ld    r2, 8(r9)      ; r2 = callee TOC (8-byte load)
bctrl
```

Five instructions plus TOC save, using `ld` (64-bit load) instead of
`lwz`. Works with the transitional output. Breaks if `.opd` entries are
shrunk without a GCC change.

---

## 2. `.sys_proc_prx_param` — process parameter block

Fixed 64-byte (`0x40`) section every Lv-2 process includes. The
loader parses it to learn the SDK version, library range markers,
and process capability flags.

```
offset  size  field                        value / reloc
------  ----  ---------------------------  -----------------------------------
0x00    4     size                         0x00000040 (literal; matches sh_size)
0x04    4     magic                        0x1b434cec (literal)
0x08    4     version                      0x00000004 (literal)
0x0c    4     sdk_version                  BCD-encoded SDK version word
                                           (for example 0x00475001 for 4.75.001)
0x10    4     lib_ent.begin                R_PPC64_ADDR32 __begin_of_section_lib_ent + 4
0x14    4     lib_ent.end                  R_PPC64_ADDR32 __end_of_section_lib_ent
0x18    4     lib_stub.begin               R_PPC64_ADDR32 __begin_of_section_lib_stub + 4
0x1c    4     lib_stub.end                 R_PPC64_ADDR32 __end_of_section_lib_stub
0x20    4     flags                        bit-field (see §2.1 below);
                                           observed 0x01010000 in shipping
                                           binaries, 0x00000000 in minimal
                                           test samples
0x24    4     ppu_guid_addr                R_PPC64_ADDR32 __PPU_GUID
0x28    4     sys_process_enable_addr      R_PPC64_ADDR32 __sys_process_enable_*
                                           (the trailing token is a publisher /
                                           authorization code, mangled into the
                                           symbol name at link time)
0x2c    4     reserved                     0 (literal)
0x30   16     reserved / padding           0 (literal)
```

Section attributes:

- `sh_type = SHT_PROGBITS`
- `sh_flags = SHF_ALLOC` (`A`)
- `sh_addralign = 4`
- Mapped by the linker script to a program header of type
  `0x60000002` (a Lv-2-specific `PT_*` value) so the loader can
  find it via the phdr table.

All pointer fields are `R_PPC64_ADDR32` — 32-bit userland EAs. No
`ADDR64` relocations appear in this section.

### 2.1 Flags at offset 0x20

Empirically `0x01010000` in shipping binaries, `0x00000000` in our
minimal test samples. Full bit semantics not yet characterized;
known-safe to emit as zero for homebrew that does not need
publisher-feature opt-ins.

### 2.2 The `+4` addends on the begin markers

The `lib_ent.begin` and `lib_stub.begin` fields carry `+4` addends.
This accounts for a 4-byte alignment / prefix header at the start
of the `.lib.ent.top` and `.lib.stub.top` sections. `runtime/lv2/crt/lv2-sprx.S`
emits that 4-byte `.long 0` prefix explicitly so the addends
resolve correctly.

If the prefix is not emitted, the addend would point past the
section into unrelated data and the loader would fail to find the
first library entry.

---

## 3. `.lib.ent` family — exported library metadata

A process / PRX declares the libraries it exports through three
sub-sections that the linker script concatenates:

- `.lib.ent.top` — 4-byte prefix header (a `.long 0`).
- `.lib.ent`    — one `LibEnt` record per exported library.
- `.lib.ent.btm` — 4-byte trailer sentinel.

The bookends are what `__begin_of_section_lib_ent` and
`__end_of_section_lib_ent` are defined to point at. The
`.sys_proc_prx_param` fields at `0x10` / `0x14` carry the range.

`.lib.ent` is empty for a normal EXEC (processes don't export
libraries). PRX modules populate it.

---

## 4. `.lib.stub` family — imported function metadata

Sibling of `.lib.ent` on the import side:

- `.lib.stub.top` — 4-byte prefix header.
- `.lib.stub`    — one `Stub` record per imported library.
- `.lib.stub.btm` — 4-byte trailer sentinel.

Every process / PRX populates this: it describes the Lv-2 libraries
to dynamically bind at load time.

### 4.1 `Stub` record layout (48 bytes)

```c
typedef struct {
    uint32_t header1;     // 0x00 — library header tag
    uint16_t header2;     // 0x04
    uint16_t imports;     // 0x06 — count of FNID entries for this library
    uint32_t zero1;       // 0x08
    uint32_t zero2;       // 0x0c
    uint32_t name;        // 0x10 — EA of library-name string
    uint32_t fnid;        // 0x14 — EA of first FNID entry for this library
    uint32_t stub;        // 0x18 — EA of first stub-address slot
    uint32_t zero3;       // 0x1c
    uint32_t zero4;       // 0x20
    uint32_t zero5;       // 0x24
    uint32_t zero6;       // 0x28
} Stub;                   // = 44 bytes + 4 alignment padding → 48 bytes in the table
```

(Header layout verified against the source of our post-link
`tools/sprx-linker`, which reads this struct to patch stub import
counts.)

The `imports` count must match the number of FNID entries this
stub record covers in `.rodata.sceFNID`. It is not always known at
compile time — the linker and post-link tools cooperate to fill it
in:

- The compiler / stub archive emits `imports = 0` as a placeholder.
- `sprx-linker` walks `.lib.stub` after link, for each record finds
  the next record's `fnid` (or end-of-section), computes
  `(next_fnid_ea - this_fnid_ea) / 4`, and writes that count into
  the record's `imports` field.

---

## 5. `.sceStub.text` — call trampolines for imported functions

Each imported function has one stub trampoline in this section.
Purpose: at call time, load the resolved function address from a
slot in `.data.sceFStub`, then jump through it.

### 5.1 Trampoline instruction sequence (32 bytes per stub)

```asm
li    r12, 0                ; r12 = 0
oris  r12, r12, DATA_HI     ; r12 = high 16 bits of .data.sceFStub base
lwz   r12, DATA_LO(r12)     ; r12 = *(.data.sceFStub + N) — EA of resolved descriptor
std   r2, 40(r1)            ; save caller TOC
lwz   r0, 0(r12)            ; r0 = entry EA (read from the 8-byte descriptor)
lwz   r2, 4(r12)            ; r2 = callee TOC
mtctr r0
bctr                        ; branch (no link — stubs preserve LR)
```

Eight instructions × 4 bytes = 32 bytes per stub. The `DATA_HI` and
`DATA_LO` immediates are filled by the linker to reach the
function's `.data.sceFStub` slot.

### 5.2 Relationship to `.opd` and FNID tables

At load time the Lv-2 loader:

1. For each `Stub` record in `.lib.stub`, look up the library by
   its header tag.
2. For each FNID in the `imports` range, look up the function by
   (library tag, FNID).
3. Write the resolved function's `.opd` descriptor address into the
   corresponding slot in `.data.sceFStub`.

From then on, any call to the imported function goes through the
stub trampoline, which dereferences the resolved descriptor EA
from `.data.sceFStub` and jumps through it using the standard
2-word compact-descriptor read.

This is the same indirect-call sequence used for regular C
function-pointer calls — `.sceStub.text` is just a code-level
construct that the linker puts between the caller and the final
indirect dispatch, so per-call-site assembly doesn't need to load
from `.data.sceFStub` explicitly.

---

## 6. Relocation quick reference

The relocation types that appear in Lv-2 user-mode ELF outputs:

| Type                | Used for                                         |
|---------------------|--------------------------------------------------|
| `R_PPC64_ADDR32`    | `.opd` entry fields, `.sys_proc_prx_param` fields, `.data.sceFStub` slots, function-pointer arrays |
| `R_PPC64_ADDR16_*`  | Short displacements in code (`lis/ori` pairs, addi immediates) |
| `R_PPC64_REL24`     | `bl` branch-with-link targets (26-bit relative) |
| `R_PPC64_TOC16*`    | TOC-relative loads (`.toc@toc@l` / `.toc@toc@ha`) |
| `R_PPC64_REL32`     | Rare; PC-relative 32-bit (exception-handling tables) |
| `R_PPC64_TLSGD`     | TLS general-dynamic (rare in Lv-2 user code; also appears as a size-0 marker in some reference-produced `.opd` entries — see §1.3) |

`R_PPC64_ADDR64` is permitted in `.data` / `.toc` payload only when
the referent is a 64-bit absolute address (rare in userland). It
MUST NOT appear in `.opd` or `.sys_proc_prx_param`.

---

## 7. ELF header — the other fingerprint the loader checks

A conformant Lv-2 ELF has these identity bytes:

```
e_ident[EI_MAG0..3]    = {0x7f, 'E', 'L', 'F'}
e_ident[EI_CLASS]      = ELFCLASS64  (2)
e_ident[EI_DATA]       = ELFDATA2MSB (2)    — big-endian
e_ident[EI_VERSION]    = EV_CURRENT  (1)
e_ident[EI_OSABI]      = 0x66                — the Lv-2 OS/ABI marker
e_ident[EI_ABIVERSION] = 0
e_machine              = EM_PPC64    (21)
e_flags                = 0x01000000           — Lv-2 ABI flag bit
```

Upstream readelf prints the OS/ABI byte as `<unknown: 66>` because
66 is not a registered value in the ELF standard. Our forked
binutils tooling could (but does not yet) render it as
`CellOS Lv-2`.

`e_flags = 0x01000000` is assigned (not OR'd): Lv-2 does not use
the standard `EF_PPC64_ABI` low bits that mark ELFv1 / ELFv2. The
binutils patch `patches/ppu/binutils-2.42/0001-elf64-ppc-tag-cellos-lv2-headers.patch`
clears those bits by assigning the full 32-bit flag word.

---

## 8. Program header types specific to Lv-2

Two PT_* values appear in Lv-2 ELFs that are not in the standard:

| p_type       | Points to                 | Purpose                           |
|--------------|---------------------------|-----------------------------------|
| `0x60000001` | `.sys_proc_param` section  | Legacy process parameter phdr (40-byte layout, where present) |
| `0x60000002` | `.sys_proc_prx_param` section | Current process parameter phdr (64-byte layout) |

Both live in the `PT_LOOS..PT_HIOS` range (`0x60000000..0x6fffffff`)
reserved for OS-specific phdr types. The linker script (currently
`src/ps3dev/PSL1GHT/ppu/crt/lv2.ld`, eventually
`runtime/lv2/linker/lv2.ld`) declares both PHDRS and assigns the
sections to them.

The loader walks the phdr table and finds the process parameter
block by its `p_type`, so even a stripped binary (without section
headers) can still be dispatched.

---

## 9. Cross-references

- `docs/abi/cellos-lv2-abi-spec.md` — normative binary contract
  (what MUST be true).
- `docs/abi/toolchain-architecture.md` — which toolchain component
  emits or consumes which section.
- `docs/abi/compact-opd-migration.md` — coordinated GCC + binutils
  changes for native 8-byte `.opd` emission.
- `tools/abi-verify/src/lib.rs` — structural checker that enforces
  these layouts on built artifacts.
- `tools/sprx-linker/sprx-linker.c` — post-link fix-ups (`.lib.stub`
  import counts). The transitional `.opd` packing step has been
  retired; native GCC emission is used directly.
- `runtime/lv2/crt/lv2-sprx.S` — our native `.sys_proc_prx_param`
  emitter.
