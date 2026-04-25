# CellOS Lv-2 ELF section reference

Byte-level reference for the Lv-2-specific ELF sections and tables that PS3DK
currently emits or consumes.

Companion documents:

- `docs/abi/cellos-lv2-abi-spec.md` — normative ABI contract.
- `docs/abi/toolchain-architecture.md` — which toolchain component emits or
  consumes each section.
- `docs/abi/compact-opd-migration.md` — status of the native compact `.opd`
  migration.

All offsets are from the section base unless stated otherwise. All multi-byte
values are big-endian (`ELFDATA2MSB`).

---

## 1. `.opd` — compact function descriptors

`.opd` holds one compact descriptor for each function whose address is needed.
The CellOS Lv-2 ABI uses an 8-byte descriptor, not the standard 24-byte PPC64
ELFv1 descriptor.

```text
offset  size  contents                         relocation / source
------  ----  -------------------------------  -------------------------------
0x00    4     function entry-point EA          32-bit relocation to code symbol
0x04    4     module TOC base EA               32-bit relocation/marker to TOC
```

Section attributes for native compact output:

- `sh_type = SHT_PROGBITS`
- `sh_flags = SHF_WRITE | SHF_ALLOC` (`WA`)
- `sh_addralign = 4`
- `sh_entsize = 0`

`sh_entsize` remains zero by convention even though the descriptor stride is
fixed at 8 bytes.

### 1.1 Interpretation

A C function pointer is a native 64-bit PPU pointer whose value is the EA of an
8-byte descriptor.

The descriptor contains two 32-bit effective addresses:

1. the function entry point in executable code;
2. the module TOC base used by code in that module.

Every function in a module normally carries the same TOC value. The TOC field
is still stored in each descriptor so an indirect call can switch `r2` to the
callee module's TOC without a separate trampoline.

### 1.2 Indirect-call sequence

Given a descriptor pointer in `r9`, the compact call sequence is:

```asm
std   r2, 40(r1)     ; save caller TOC
lwz   r0, 0(r9)      ; r0 = entry EA
mtctr r0
lwz   r2, 4(r9)      ; r2 = callee TOC base
bctrl
ld    r2, 40(r1)     ; restore caller TOC when needed
```

`lwz` is intentional. The field is a 32-bit userland EA, and the instruction
zero-extends into the 64-bit register.

### 1.3 TOC-field encodings

The descriptor tail must resolve to the module TOC base EA in final output.

Object files may reach that final value through either of these encodings:

- a direct 32-bit relocation against `.TOC.` or the linker-provided TOC base;
- the CellOS-specific `R_PPC64_TLSGD *ABS*` marker described in the normative
  ABI spec.

The linked binary contract is the same either way: bytes `0x04..0x07` contain
the 32-bit TOC base EA. `R_PPC64_ADDR64` is not valid for `.opd`.

### 1.4 Retired legacy compatibility form

The old PSL1GHT-compatible transitional layout was a 24-byte PPC64 ELFv1
descriptor with an extra compact descriptor packed into the environment slot:

```text
offset  size  contents
------  ----  ---------------------------------------------------------
0x00    8     64-bit entry address
0x08    8     64-bit TOC address
0x10    8     packed compact pair: { entry_lo32, toc_lo32 }
```

Native PS3DK output no longer uses that form. It exists only as a compatibility
case inside `tools/sprx-linker` for legacy 24-byte input sections. Compact
sections are identified by 4-byte `.opd` alignment and are left untouched.

---

## 2. `.sys_proc_prx_param` — process parameter block

Fixed 64-byte (`0x40`) section every Lv-2 process includes. The loader parses
it to learn the SDK version, library range markers, and process capability
flags.

```text
offset  size  field                        value / relocation
------  ----  ---------------------------  -----------------------------------
0x00    4     size                         0x00000040
0x04    4     magic                        0x1b434cec
0x08    4     version                      0x00000004
0x0c    4     sdk_version                  BCD SDK version, e.g. 0x00475001
0x10    4     lib_ent.begin                __begin_of_section_lib_ent + 4
0x14    4     lib_ent.end                  __end_of_section_lib_ent
0x18    4     lib_stub.begin               __begin_of_section_lib_stub + 4
0x1c    4     lib_stub.end                 __end_of_section_lib_stub
0x20    4     abi_version                  0x01010000
0x24    4     ppu_guid_addr                __PPU_GUID
0x28    4     sys_process_enable_addr      __sys_process_enable_*
0x2c    4     reserved                     0
0x30   16     reserved / padding           0
```

Section attributes:

- `sh_type = SHT_PROGBITS`
- `sh_flags = SHF_ALLOC` (`A`)
- `sh_addralign = 4`
- mapped to the Lv-2-specific program header type `0x60000002`

Every pointer-like field is a 32-bit EA relocation. This section must not use
64-bit address relocations.

### 2.1 `abi_version` at offset `0x20`

The current normative value is `0x01010000`. Earlier minimal experiments used
zero, but reference SDK binaries and runtime testing show that the non-zero
value is required for the loader path PS3DK targets.

### 2.2 The `+4` addends on begin markers

The `lib_ent.begin` and `lib_stub.begin` fields carry `+4` addends because the
`.lib.ent.top` and `.lib.stub.top` sections begin with a 4-byte prefix word.

`runtime/lv2/crt/lv2-sprx.S` emits those prefix words. Removing them would make
the process parameter block point past the beginning of the loader-visible
library tables.

---

## 3. `.lib.ent` family — exported library metadata

Exported libraries are described through a concatenated range:

```text
.lib.ent.top   4-byte prefix word
.lib.ent       exported library records
.lib.ent.btm   trailer / sentinel
```

A normal executable often has an empty `.lib.ent` body. PRX modules populate it
with one record per exported library.

The `.sys_proc_prx_param` block points to the range using
`__begin_of_section_lib_ent + 4` and `__end_of_section_lib_ent`.

---

## 4. `.lib.stub` family — imported library metadata

Imported libraries are described through the sibling range:

```text
.lib.stub.top   4-byte prefix word
.lib.stub       one Stub record per imported library
.lib.stub.btm   trailer / sentinel
```

Every executable or PRX that imports Lv-2 libraries has this range.

### 4.1 `Stub` record layout

`tools/sprx-linker` treats each `.lib.stub` entry as the packed structure below:

```c
typedef struct {
    uint32_t header1;
    uint16_t header2;
    uint16_t imports;
    uint32_t zero1;
    uint32_t zero2;
    uint32_t name;
    uint32_t fnid;
    uint32_t stub;
    uint32_t zero3;
    uint32_t zero4;
    uint32_t zero5;
    uint32_t zero6;
} Stub;
```

The structure is 44 bytes packed. The table layout may include alignment between
records depending on how the surrounding assembly emits it; `sprx-linker` uses
the packed structure size when walking the section.

The `imports` field is filled post-link. `sprx-linker` compares each record's
FNID start address with the next record's FNID start address, or with the end of
`.rodata.sceFNID`, then writes the number of 4-byte FNID entries covered by the
record.

---

## 5. `.sceStub.text` and `.data.sceFStub` — imported function dispatch

Each imported function has:

- an FNID entry in `.rodata.sceFNID`;
- a loader-patched slot in `.data.sceFStub`;
- a trampoline in `.sceStub.text`.

At load time, the Lv-2 loader resolves each imported function by library NID and
function NID, then writes the resolved function descriptor EA into the matching
`.data.sceFStub` slot.

The trampoline then loads that descriptor EA and jumps through the same compact
two-word descriptor contract used by normal C function-pointer calls.

A compact-descriptor import trampoline conceptually does:

```asm
load address of .data.sceFStub slot
lwz/load slot value          ; descriptor EA
std   r2, 40(r1)             ; save caller TOC
lwz   r0, 0(descriptor)      ; entry EA
lwz   r2, 4(descriptor)      ; callee TOC
mtctr r0
bctr
```

The exact materialization of the `.data.sceFStub` slot address is generated by
the stub archive/nidgen path.

---

## 6. Relocation quick reference

Relocation types that appear in conforming Lv-2 user-mode ELF outputs:

| Type | Used for |
|---|---|
| `R_PPC64_ADDR32` | 32-bit EA fields, process-param pointers, descriptor entry fields, stub slots |
| `R_PPC64_ADDR16_*` | split immediate materialization in code |
| `R_PPC64_REL24` | branch-with-link targets |
| `R_PPC64_TOC16*` | TOC-relative accesses |
| `R_PPC64_REL32` | PC-relative 32-bit references, including some EH data |
| `R_PPC64_TLSGD` | CellOS compact-`.opd` TOC marker where emitted |

`R_PPC64_ADDR64` may appear in ordinary data only when a real 64-bit absolute
value is intended. It must not appear in `.opd` or `.sys_proc_prx_param`.

---

## 7. ELF header identity

A conforming Lv-2 PPU ELF has:

```text
e_ident[EI_MAG0..3]    = {0x7f, 'E', 'L', 'F'}
e_ident[EI_CLASS]      = ELFCLASS64
e_ident[EI_DATA]       = ELFDATA2MSB
e_ident[EI_VERSION]    = EV_CURRENT
e_ident[EI_OSABI]      = 0x66
e_ident[EI_ABIVERSION] = 0
e_machine              = EM_PPC64
e_flags                = 0x00000000
```

Older PS3DK notes documented `e_flags = 0x01000000`; the current ABI spec was
corrected after comparing against reference SDK binaries. The OS/ABI byte is
the CellOS Lv-2 fingerprint. Upstream `readelf` may print it as
`<unknown: 66>`.

---

## 8. Program header types specific to Lv-2

Lv-2 uses OS-specific program header types in the `PT_LOOS..PT_HIOS` range:

| `p_type` | Points to | Purpose |
|---|---|---|
| `0x60000001` | `.sys_proc_param` | legacy process parameter block, where present |
| `0x60000002` | `.sys_proc_prx_param` | current 64-byte process parameter block |

The loader can find these blocks through the program header table even if
section headers are stripped.

---

## 9. Cross-references

- `docs/abi/cellos-lv2-abi-spec.md` — normative binary contract.
- `docs/abi/toolchain-architecture.md` — producer/consumer map.
- `docs/abi/compact-opd-migration.md` — compact `.opd` implementation status.
- `tools/abi-verify/src/lib.rs` — structural conformance checker.
- `tools/sprx-linker/sprx-linker.c` — post-link `.lib.stub` import-count fixups
  and legacy `.opd` compatibility fallback.
- `runtime/lv2/crt/lv2-sprx.S` — native `.sys_proc_prx_param` emitter.
- `runtime/lv2/crt/crt0.S` — compact `__start` descriptor and process entry.
- `runtime/lv2/crt/lv2.ld` — native linker script with `.opd ALIGN(4)`.
