# CellOS Lv-2 PPU ABI — normative specification

Authoritative binary contract that every toolchain component in this project
(GCC PPU target, binutils, newlib, CRT, runtime, SDK, nidgen, stub archives)
must emit or accept.

Conformance is enforced by `tools/abi-verify`. Every shipped binary or sample
output must pass its invariant checks and diff cleanly against a matching
fixture in `tests/abi/fixtures/`.

The spec was derived empirically from reference-SDK ELF artifacts and is
frozen here so downstream code targets it directly, not a reverse-engineering
narrative. Discovery notes live in per-session memory, not in this tree.

---

## 1. ELF file identity

All PPU user-mode ELF outputs (REL objects, DYN shared objects, EXEC
binaries, SELF/SPRX payloads prior to post-processing) MUST satisfy:

| Field            | Required value                      |
|------------------|-------------------------------------|
| `EI_CLASS`       | `ELFCLASS64` (2)                    |
| `EI_DATA`        | `ELFDATA2MSB` (big-endian)          |
| `EI_VERSION`     | `EV_CURRENT` (1)                    |
| `EI_OSABI`       | `0x66` (CellOS Lv-2)                |
| `EI_ABIVERSION`  | `0`                                 |
| `e_machine`      | `EM_PPC64` (21)                     |
| `e_flags`        | `0x01000000`                        |

Upstream readelf prints the OS/ABI byte as `<unknown: 66>` — this is expected.
Our fork of binutils (when added) SHOULD render it as `CellOS Lv-2`.

---

## 2. Compact function-descriptor format (`.opd`)

CellOS Lv-2 uses an **8-byte compact function descriptor**, not the
PowerPC ELFv1 24-byte 3-doubleword form. Every `.opd` entry is exactly
8 bytes and is laid out as:

```
offset  size  contents
------  ----  -----------------------------------------------
0x00    4     Function entry-point EA (32-bit) — R_PPC64_ADDR32
0x04    4     TOC/module token, filled at link time
              — R_PPC64_TLSGD marker (size-0 reloc, addend 0)
```

Normative rules:

1. `.opd` sections have `sh_entsize = 0` (variable-format convention) but
   every descriptor has a fixed 8-byte stride.
2. `sh_addralign` for `.opd` is 4.
3. Each descriptor's head reloc is `R_PPC64_ADDR32` against a defined local
   or external function symbol (by convention prefixed `.funcname` in the
   reference tree; our toolchain may emit with or without the dot prefix as
   long as the symbol resolves).
4. Each descriptor's tail reloc is `R_PPC64_TLSGD` with no symbol and addend 0
   — a marker the loader uses to fill in the 4-byte TOC/module token.
5. No entry uses `R_PPC64_ADDR64`. Any 64-bit descriptor reloc is a conformance
   error and indicates upstream-ELFv1 leakage.

### Implementation consequences

- The PSL1GHT `__get_opd32` helper exists because PSL1GHT emits 24-byte
  upstream-ELFv1 descriptors and then laundering extracts an 8-byte view.
  Native code MUST emit 8-byte descriptors directly; `__get_opd32` has no
  place in strict-profile binaries.
- GCC's default PPC64 backend emits 24-byte descriptors. The binutils
  linker (or a small post-process step) is responsible for rewriting them
  to the compact form, or GCC must be told to emit 8-byte entries via a
  target-specific hook. This is open work; see the `feature/lv2-abi-native`
  branch plan.

---

## 3. Process parameter section (`.sys_proc_prx_param`)

Fixed 64-byte (`0x40`) section, `sh_addralign = 4`, `sh_type = PROGBITS`,
flags `SHF_ALLOC`. Binary layout:

```
offset  size  field               value / reloc
------  ----  ------------------  --------------------------------------
0x00    4     size                0x00000040 (matches sh_size)
0x04    4     magic               0x1b434cec
0x08    4     version             0x00000004
0x0c    4     sdk_version         BCD SDK version (e.g. 0x00475001)
0x10    4     lib_ent.begin       R_PPC64_ADDR32 __begin_of_section_lib_ent + 4
0x14    4     lib_ent.end         R_PPC64_ADDR32 __end_of_section_lib_ent
0x18    4     lib_stub.begin      R_PPC64_ADDR32 __begin_of_section_lib_stub + 4
0x1c    4     lib_stub.end        R_PPC64_ADDR32 __end_of_section_lib_stub
0x20    4     reserved / flags    0x00000000
0x24    4     ppu_guid_addr       R_PPC64_ADDR32 __PPU_GUID
0x28    4     sys_process_enable  R_PPC64_ADDR32 __sys_process_enable_*
0x2c    4     reserved            0
0x30    16    reserved / padded   zero
```

Normative rules:

1. `size` and `magic` are literal values stored in the section bytes at
   link time; they are **not** relocated.
2. Every pointer field is a `R_PPC64_ADDR32` relocation — no `ADDR64`.
3. The `+4` addends on the `lib_ent.begin` and `lib_stub.begin` fields
   account for a 4-byte section header prefix at the start of
   `.lib.ent.top` / `.lib.stub.top`; samples MUST preserve that layout.
4. The `__sys_process_enable_*` symbol name is compile-time mangled with
   a publisher/company token (e.g. `__sys_process_enable_cp_43454c4c__`
   encodes ASCII `CELL`). Our toolchain selects the token at link time
   via a linker-script symbol; the value is not fixed by this spec.

---

## 4. Pointer and addressing model

CellOS Lv-2 is a 64-bit ABI with a 32-bit userland effective-address
space. This spec treats those as two distinct notions:

- **Native pointers**: C-level `void *`, `T *`, and all derived pointer
  types are 64-bit (`DImode`). Public API struct fields MUST use native
  pointers, never `__attribute__((mode(SI)))`. Example: `CellGcmConfig`
  has `void *localAddress` at 8 bytes, not 4.

- **Effective-address fields (`lv2_ea32_t`)**: a small number of
  ABI-fixed metadata fields hold a 32-bit EA value as a `uint32_t`. The
  canonical typedef is `lv2_ea32_t`, defined in
  `sdk/include/sys/lv2_types.h`. Examples: `sys_process_param_t::crash_dump_param_addr`,
  every reloc target in `.sys_proc_prx_param`, every `.opd` entry-point
  slot.

Normative rules:

1. `lv2_ea32_t` is `uint32_t`. Conversion helpers (`lv2_ea32_pack(ptr)`,
   `lv2_ea32_expand(ea)`) do explicit narrow/widen with assertable range
   checks; implicit pointer → `uint32_t` narrowing is flagged by
   `-Wps3-ea32-truncation` when that warning is introduced.
2. No public API in `cell/*.h` or `sys/*.h` SHALL declare a `void *`
   field with `mode(SI)`. Any such declaration is a conformance bug.
3. The GCC target hook override that currently accepts SImode pointers
   on PPU64 (see `patches/ppu/gcc-12.x/0005-rs6000-cell64lv2-pointer-mode.patch`)
   is a transitional workaround. Once `lv2_ea32_t` replaces every
   legitimate use, the hook override is removed and strict-profile
   binaries MUST build without it.

### 4.1 Kernel-side struct ABI (settled 2026-04-20)

PSL1GHT declares several kernel-interface structs (notably
`gcmConfiguration`) with `void * __attribute__((mode(SI)))` fields,
yielding a packed 24-byte struct with 32-bit pointers. Reference SDK
headers declare the analogous struct (`CellGcmConfig`) as 32 bytes
with native 64-bit pointers.

`samples/toolchain/gcm-config-abi/` was built to settle which layout
the LV2 kernel actually writes. The test allocates a 64-byte
`0xAA`-filled buffer, calls `cellGcmGetConfiguration` via the nidgen
NID stub, and dumps the buffer. RPCS3 result:

```
[00..07] c0 00 00 00 40 10 00 00   localAddress=0xC0000000, ioAddress=0x40100000
[08..15] 0f 90 00 00 02 00 00 00   localSize=~249 MB, ioSize=32 MB
[16..23] 26 be 36 80 1d cd 65 00   memoryFreq=650 MHz, coreFreq=500 MHz
[24..31] aa aa aa aa aa aa aa aa   UNTOUCHED
[32..63] aa aa aa ...              UNTOUCHED
```

All 24 bytes decode to physically meaningful PS3 RSX values
(GDDR3 clock is 650 MHz, RSX core is 500 MHz, `0xC0000000` is the
RSX local-memory base in PPU EA space). Bytes 24-63 remain at the
sentinel — the kernel writes **exactly 24 bytes**.

**Normative consequence:**

- The public API in `cell/*.h` exposes the 32-byte `CellGcmConfig`
  with native 64-bit pointers (matches the reference SDK header
  caller-contract and keeps user code portable).
- The kernel-interface struct the Lv-2 syscall writes is 24 bytes
  with 32-bit pointers.
- The widener in `sdk/include/cell/gcm.h:226-240` IS a structural
  bridge and MUST remain in place. It zero-extends the two
  32-bit EA fields and populates the trailing `memoryFrequency`
  / `coreFrequency` words. This is correct, documented behaviour,
  not a workaround.
- Reference-SDK-visible `memoryFrequency` / `coreFrequency` fields
  in the 32-byte struct come from the same 24-byte syscall payload,
  just mapped into the trailing portion of the wider struct.

Subsequent Phase 3 work on libgcm_sys internals must preserve the
24-byte kernel calling convention. The `mode(SI)` attribute on
`gcmConfiguration`'s pointer fields is semantically correct — it
captures the kernel's 32-bit EA contract. Our own `gcmConfiguration`
type (if/when we ship one) may use `lv2_ea32_t` instead of
`void * mode(SI)` to avoid leaning on the GCC pointer-mode patch,
but the field widths stay 32 bits.

---

## 5. Relocation class contract

Only a small set of PPC64 relocation types appear in conformant
CellOS Lv-2 objects. `abi-verify` flags any relocation outside this set.

| Type                   | Use                                          |
|------------------------|----------------------------------------------|
| `R_PPC64_ADDR32`       | `.opd` entry EA, `.sys_proc_prx_param` fields, call/ref within 32-bit EA range |
| `R_PPC64_ADDR16_*`     | Short displacements in code                   |
| `R_PPC64_REL24`        | Branch relocations                            |
| `R_PPC64_TOC16*`       | TOC-relative references                       |
| `R_PPC64_TLSGD`        | `.opd` descriptor token marker (size 0)       |
| `R_PPC64_REL32`        | PC-relative 32-bit references                 |

`R_PPC64_ADDR64` is permitted in `.data` / `.toc` payload only when the
referent is a 64-bit absolute address (rare in userland). It MUST NOT
appear in `.opd` or `.sys_proc_prx_param`.

---

## 6. Section naming conventions

Minimum required sections for a loadable Lv-2 user binary:

```
.text            code
.data / .bss     initialized / zero data
.toc             TOC entries
.opd             compact function descriptors
.sys_proc_prx_param  process parameter block (PRX-enabled processes)
.lib.ent.top / .lib.ent / .lib.ent.btm      exported library table
.lib.stub.top / .lib.stub / .lib.stub.btm   imported stub table
```

`.sys_proc_param` (without the `_prx_` infix) is the legacy name used by
some non-PRX binaries; our toolchain emits `.sys_proc_prx_param` by
default and treats `.sys_proc_param` as a deprecated alias.

---

## 7. Conformance test matrix

Every release candidate MUST:

1. Build `samples/toolchain/hello-ppu-abi-check` under `-mps3-runtime=native`
   with zero warnings and have `abi-verify check` emit all PASS lines.
2. Diff the sample's emitted manifest against
   `tests/abi/fixtures/crt1.json` on every common field: ELF header,
   `.opd` entry shape, `.sys_proc_prx_param` size/magic/version.
3. Link and run the sample in RPCS3 with process start, exit-handler
   fire, and clean exit observed in the emulator log.

Deviations from this spec require either (a) a fixture update with
documented rationale, or (b) a change to `abi-verify` with a regression
test. The spec is authoritative — tools and code change to match it,
not the other way around.
