# CellOS Lv-2 PPU ABI - normative specification

Authoritative binary contract that every toolchain component in this project
(GCC PPU target, binutils, newlib, CRT, runtime, SDK, nidgen, stub archives)
must emit or accept.

Conformance is enforced by `tools/abi-verify`. Every shipped binary or sample
output must pass its invariant checks and diff cleanly against a matching
fixture in `tests/abi/fixtures/`.

This file is the normative binary contract. Discovery notes and historical
rationale belong outside the ABI spec.

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
| `e_flags`        | `0x00000000`                        |

Upstream readelf prints the OS/ABI byte as `<unknown: 66>` - this is expected.
Our fork of binutils (when added) SHOULD render it as `CellOS Lv-2`.

The `e_flags` field is zero for conforming CellOS Lv-2 PPU user-mode
outputs. The CellOS identity is carried by `EI_OSABI = 0x66`.

---

## 2. Compact function-descriptor format (`.opd`)

Every `.opd` entry is exactly 8 bytes and is laid out as:

```
offset  size  contents
------  ----  -----------------------------------------------
0x00    4     Function entry-point EA (32-bit) - R_PPC64_ADDR32
0x04    4     Module TOC base EA (32-bit) - R_PPC64_TLSGD *ABS* marker
```

The `R_PPC64_TLSGD *ABS*` relocation at offset +4 is a binutils hook that
resolves at link time to the module's TOC base EA. Every descriptor in a
given module carries the same TOC value (module-level constant, not per-function).

Normative rules:

1. `.opd` sections have `sh_entsize = 0` (variable-format convention) but
   every descriptor has a fixed 8-byte stride.
2. `sh_addralign` for `.opd` is 4.
3. Each descriptor's head reloc is `R_PPC64_ADDR32` against a defined local
   or external function symbol (by convention prefixed `.funcname` in the
   reference tree; our toolchain may emit with or without the dot prefix as
   long as the symbol resolves).
4. Each descriptor's tail reloc is `R_PPC64_TLSGD` with no symbol and addend 0
   - a link-time directive that writes the module TOC base EA into the slot.
5. No entry uses `R_PPC64_ADDR64`. Any 64-bit descriptor reloc is a conformance
   error and indicates upstream-ELFv1 leakage.

### Implementation consequences

- The PSL1GHT `__get_opd32` helper can be retired; native code MUST emit
  8-byte descriptors directly.
- GCC's `-mps3-opd-compact` flag (or default on `powerpc64-ps3-elf`) emits
  the compact form natively. The binutils linker resolves `R_PPC64_TLSGD`
  by writing the module TOC base EA at link time.
- `lv2_fn_to_callback_ea(fn)` is now a bare cast - the `+16` offset is
  obsolete and should be removed from `<sys/lv2_types.h>`.

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
0x20    4     abi_version         0x01010000 (REQUIRED - see rule 5)
0x24    4     ppu_guid_addr       R_PPC64_ADDR32 __PPU_GUID
0x28    4     sys_process_enable  R_PPC64_ADDR32 __sys_process_enable_*
0x2c    4     reserved            0
0x30    16    reserved / padded   zero
```

Normative rules:

1. `size` and `magic` are literal values stored in the section bytes at
   link time; they are **not** relocated.
2. Every pointer field is a `R_PPC64_ADDR32` relocation - no `ADDR64`.
3. The `+4` addends on the `lib_ent.begin` and `lib_stub.begin` fields
   account for a 4-byte section header prefix at the start of
   `.lib.ent.top` / `.lib.stub.top`; samples MUST preserve that layout.
4. The `__sys_process_enable_*` symbol name is compile-time mangled with
   a publisher/company token (e.g. `__sys_process_enable_cp_43454c4c__`
   encodes ASCII `CELL`). Our toolchain selects the token at link time
   via a linker-script symbol; the value is not fixed by this spec.
5. `abi_version` at offset `0x20` is the 32-bit literal `0x01010000`.
   Zero here is not a conforming process-parameter block and can stall
   the loader before the module entry point is invoked. The normative
   requirement is the literal value.

---

## 4. Pointer and addressing model — ELF64 + ILP32 hybrid

The default PPU C data model is **ILP32 with a 64-bit ELF wrapper**
(Pmode = SImode, ELFCLASS64, EM_PPC64). Pointers are 32-bit on the
wire, registers are 64-bit, and the ELF header reports ELF64.
Multilib `-mlp64` opt-in flips Pmode to DImode for LP64 user code.

The default `powerpc64-ps3-elf-gcc` invocation therefore produces
ELF64 binaries with `sizeof(void *) == 4`. That is the data model used
by the CellOS SPRX runtime, LV2 syscall layer, CRT0 chain, compact OPD
descriptors, and the published `cell/*.h` struct layouts. Code written
for older PS3 SDKs should not need permissive pointer conversions just
to fit this project.

Code that wants a genuinely wide C pointer model can pass `-mlp64`.
The SDK install builds the runtime objects, native archives, and
nidgen-emitted stub archives for both data models; CMake samples pick
the correct library path from the selected compiler flags. The PRX
boundary remains a 32-bit-effective-address interface even when user
code is compiled as LP64.

For public headers, any pointer field inside a caller-allocated struct
that crosses the SPRX boundary must use `ATTRIBUTE_PRXPTR` or an
explicit `uint32_t` effective-address field. Width-sensitive integers
inside those same structs must be fixed-width (`uint32_t`, `uint64_t`,
and friends), not host C aliases such as `size_t`, when the firmware
reads a fixed-width field.

This spec treats addresses in three layers:

- **Effective addresses (`uint32_t`)**: every userland EA fits in 32
  bits; LV2 maps user processes into a 32-bit EA window.  All `.opd`
  entry-point slots, `.sys_proc_prx_param` pointer fields, GOT/TOC
  entries that hold a function or data EA, and `.lib.stub` /
  `.data.sceFStub` slots are **4-byte** absolute words with
  `R_PPC64_ADDR32` relocations.

- **C pointers (`void *`, `T *`)**: 4 bytes by default (Pmode =
  SImode under the ILP32 hybrid).  GCC emits `lwz @toc@l(r2)` to
  load function pointers from the GOT/TOC and `stw r0, X(r1)` for
  pointer spills.  No `R_PPC64_ADDR64` should appear in user-mode
  pointer emission.  Under `-mlp64`, pointers widen to 8 bytes
  (Pmode = DImode); the multilib variant lives under `lp64/` in the
  install tree.

- **Effective-address typedef (`lv2_ea32_t`)**: an explicit
  `uint32_t` typedef in `sdk/include/sys/lv2_types.h`.  Use this in
  caller-allocated kernel-facing structs whose pointer fields the
  Lv-2 syscall layer treats as 32-bit EAs regardless of whether the
  user binary is ILP32 or LP64.  Conversion helpers
  (`lv2_ea32_pack(ptr)`, `lv2_ea32_expand(ea)`) make the narrow / widen
  explicit.

Normative rules:

1. `Pmode == SImode` under the default toolchain target.  GCC must
   emit pointer materialization, indirect calls, TOC slot loads, and
   pointer-typed argument/return marshaling at 32-bit width.  The
   `-mlp64` opt-in path flips Pmode to DImode and generates the LP64
   variant in the same source tree without source-level changes.
2. **TOC slot width is 4 bytes** under Pmode = SImode.
   `output_toc` in `gcc/config/rs6000/rs6000.cc` emits `.long sym`
   (4 bytes) instead of `DOUBLE_INT_ASM_OP` / `.tc` (8 bytes) when
   `Pmode == SImode`.  `R_PPC64_ADDR32` instead of `R_PPC64_ADDR64`.
   See patches/ppu/gcc-12.x/0021-rs6000-output-toc-pmode.patch.
   *Cause:* an 8-byte slot under big-endian holds the address in the
   low 4 bytes; a `lwz @toc@l(r2)` load reads the high 4 bytes (zero)
   and the deref faults.  `libgloss/libsysbase` MUST be rebuilt with
   patched GCC so its `.toc` sections inherit the 4-byte layout.
   Objects with 8-byte ILP32 `.toc` slots are non-conforming.
3. `lv2_ea32_t` is `uint32_t`.  Width-sensitive integers
   (`size_t`, `ptrdiff_t`, `off_t`) in caller-allocated `cell/*`
   structs that cross the SPRX boundary MUST be declared `uint32_t`
   explicitly, not the LP64 default.  See
   `feedback_size_t_in_cell_structs.md` for the prior incident.
4. No public API in `cell/*.h` or `sys/*.h` SHALL declare a `void *`
   field with `mode(SI)`.  Any such declaration is a conformance bug.
5. The legacy GCC target hook override that accepted SImode pointers
   only on the LP64 path
   (`patches/ppu/gcc-12.x/0005-rs6000-cell64lv2-pointer-mode.patch`)
   is now redundant under the default ILP32 hybrid; the new path
   tracks the pointer-width axis directly via `Pmode`.

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
sentinel - the kernel writes **exactly 24 bytes**.

**Normative consequence:**

- The public API in `cell/*.h` exposes the 32-byte `CellGcmConfig`
  with native 64-bit pointers.
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

Subsequent libgcm_sys work must preserve the
24-byte kernel calling convention. The `mode(SI)` attribute on
`gcmConfiguration`'s pointer fields is semantically correct - it
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

## 5.1. SPRX import trampoline shape

Imported sysPrxForUser / cellGcmSys / cellSysmodule / etc. functions
are dispatched through per-export trampolines emitted into
`.sceStub.text` by `tools/nidgen` (or the equivalent
`sprx/common/exports.S` macro for legacy archives that have not been
folded into the nidgen flow yet).

The trampoline shape is data-model aware. ILP32 uses a frame-less
wrapping `bctrl` form so SPRX exports with more than eight arguments
see the caller's original stack-argument area. LP64 uses a bare
`bctr` tail-call form that defers TOC restoration to the call-site
nop slot rewritten by `sprxlinker --lp64`.

Normative trampoline body (per export, ILP32 hybrid):

```asm
__<name>:
    mflr   r0
    stw    r0, 24(r1)          ; LR -> caller's callee-TOC scratch slot
    stw    r2, 40(r1)          ; TOC -> caller's reserved scratch slot
    lis    r12, <name>_stub@ha
    lwz    r12, <name>_stub@l(r12)
    lwz    r0, 0(r12)          ; SPRX entry EA (compact OPD slot 0)
    lwz    r2, 4(r12)          ; SPRX TOC (compact OPD slot 4)
    mtctr  r0
    bctrl                       ; call SPRX
    lwz    r2, 40(r1)           ; restore caller TOC
    lwz    r0, 24(r1)           ; restore caller LR
    mtlr   r0
    blr
```

Normative trampoline body (per export, LP64):

```asm
__<name>:
    std    r2, 40(r1)           ; caller TOC, restored at call site
    lis    r12, <name>_stub@ha
    lwz    r12, <name>_stub@l(r12) ; 32-bit compact descriptor EA
    lwz    r0, 0(r12)           ; SPRX entry EA (compact OPD slot 0)
    lwz    r2, 4(r12)           ; SPRX TOC (compact OPD slot 4)
    mtctr  r0
    bctr                        ; tail-call SPRX
```

Normative rules:

1. **No stack-frame allocation in the trampoline.**  Any `stwu` /
   `stdu` before dispatch shifts the caller's parameter-save area.
   SPRX exports with more than eight arguments read arg9+ from that
   area; moving `r1` makes the callee read trampoline-frame garbage.
2. **ILP32 caller LR save** is `stw r0,24(r1)`, not `sp+16`.
   `sp+16` is the ELFv1 callee LR-save slot and is clobbered by the
   SPRX callee prologue.  `sp+24` is safe for the wrapping trampoline
   because the callee saves its incoming TOC in its own frame.
3. **ILP32 caller TOC save** is `stw r2,40(r1)` and must be restored
   after `bctrl`.
4. **LP64 tail-call form** saves caller TOC with `std r2,40(r1)` and
   uses `bctr`, not `bctrl`.  The caller-side post-call restore is
   emitted by the LP64 link path as `ld r2,40(r1)`.
5. **Compact OPD descriptor read** at `lwz r0, 0(r12); lwz r2,
   4(r12)` — the `.data.sceFStub` slot was written by the loader
   with the resolved 8-byte compact OPD address.
6. **FStub slot load is always 32-bit.**  Both ILP32 and LP64 use
   `lwz r12,<name>_stub@l(r12)` because `.data.sceFStub` slots are
   4-byte compact descriptor EAs under both data models.
7. **Single `.lib.stub` header per imported library** —
   liblv2.a/sprx.o (legacy PSL1GHT-style hand-rolled stubs) MUST NOT
   coexist with nidgen-emitted liblv2_stub.a in the link.  Both
   emit a `.lib.stub` header naming `sysPrxForUser`; with both in
   the link the loader resolves duplicate sysPrxForUser imports and
   the trampoline shape from each archive may differ.  As of commit
   1e076ed nidgen owns sysPrxForUser canonical firmware names plus
   PSL1GHT-style aliases (`sysLwMutexCreate` → `sys_lwmutex_create`,
   etc.) so liblv2.a's sprx.o has been retired.  Restore the strip
   of `sprx.o` whenever liblv2.a is rebuilt.

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
test. The spec is authoritative - tools and code change to match it,
not the other way around.

---

## 8. Relocatable module (PRX) layout

A PRX is a relocatable Lv-2 module produced by `tools/prx-gen` from a
`-Wl,-q` link against `runtime/lv2/crt/lv2-prx.ld` (design:
`docs/design/sprx-generation.md`; loader-side facts with public-source
citations: `docs/local/sprx-format-facts.md`). Sections 1-6 apply with the
differences below. Items marked *(R)* are what the emulator accepts and
await confirmation against the reference SDK; everything else is normative.

### 8.1 ELF identity

| Field | Required value |
|---|---|
| `EI_OSABI` | `0x66` |
| `e_type` | `0xFFA4` (`ET_SCE_PPURELEXEC`) |
| `e_entry` | `0` *(R: never read by the loader; zero by convention)* |
| `e_ehsize / e_phentsize / e_shentsize` | `64 / 56 / 64` |
| Program headers | one or more `PT_LOAD` followed by exactly one `0x700000A4` (`PT_SCE_PPURELA`) segment; **no** `PT_TLS`, `0x60000001`, `0x60000002` or `PT_NOTE` |
| `PHDR[0]` | must be the first `PT_LOAD`; its `p_paddr` = the module-info block's position in the `p_offset` frame (with `p_offset = 0` and `p_vaddr = 0`: its file offset). `p_paddr == 0` means "no module info". |

Our toolchain links every module at base 0 with a single `PT_LOAD`
(flags RWX) so that every relocation record has `index_addr == index_value
== 0` and `ptr` equal to the link-time address. Section headers are
optional; `prx-gen` preserves the input's.

### 8.2 Module-info block (`.rodata.sceModuleInfo`, symbol `__sys_prx_module_info`)

52 bytes, `sh_addralign = 4`, first section after the headers inside
`PHDR[0]`. Emitted by `lv2-prx.S`; name/version/attributes written by
`prx-gen`.

```
offset  size  field           value / reloc
------  ----  --------------  ----------------------------------------
0x00    2     attributes      0 (prx-gen --attributes)
0x02    2     version         major, minor (prx-gen --version)
0x04    28    name            NUL-padded, <= 27 chars (prx-gen --name)
0x20    4     toc             R_PPC64_ADDR32 .TOC.
0x24    4     exports_start   R_PPC64_ADDR32 __libentstart + 4
0x28    4     exports_end     R_PPC64_ADDR32 __libentend
0x2c    4     imports_start   R_PPC64_ADDR32 __libstubstart + 4
0x30    4     imports_end     R_PPC64_ADDR32 __libstubend
```

Normative rules:

1. The relocation segment is applied **before** the block is read, so
   the five pointer fields MUST each be covered by a relocation record.
   `prx-gen build` refuses a module with fewer than five (override:
   `--allow-unrelocated-module-info`, diagnostic use only).
2. There is no magic word and no `.sys_proc_prx_param` in a PRX.

### 8.3 Library records (`.lib.ent` / `.lib.stub`)

Both tables use the same 44-byte record; only the meaning of the pointer
fields differs (import side: section-reference.md §4).

```
offset  size  field         export meaning
------  ----  ------------  ----------------------------------------
0x00    1     size          0x2c (walk stride; 0 is read as 44)
0x01    1     unk0          0
0x02    2     version       1
0x04    2     attributes    0x0001 named library | 0x8000 management (0x0001 clear)
0x06    2     num_func      written at assembly time (label arithmetic)
0x08    2     num_var       0 in v1
0x0a    2     num_tlsvar    0 (loader rejects non-zero)
0x0c    4     info_hash, info_tlshash, unk1   0
0x10    4     name          R_PPC64_ADDR32 -> C string; 0 for the management record
0x14    4     nids          R_PPC64_ADDR32 -> u32[num_func + num_var]
0x18    4     addrs         R_PPC64_ADDR32 -> u32[num_func + num_var]; functions are compact-OPD addresses
0x1c    16    vnids, vstubs, unk4, unk5   0 (export side: variables are appended to nids/addrs)
```

Normative rules:

1. A record whose `attributes` has neither `0x0001` nor `0x8000` is
   skipped by the loader **without any diagnostic**. Export records are
   therefore emitted with literal attributes (`<sys/prx_module.h>`,
   `lv2-prx.S`), never derived from a count. The `.lib.stub` emitter's
   historical habit of writing the export count into offset 4 is benign
   on the import side only.
2. Every `nids`/`addrs`/`name` pointer, and every entry of `addrs`, MUST
   resolve inside a `PT_LOAD` segment; the loader dereferences them
   eagerly and aborts the load on an outside address. Consequently the
   management record MUST NOT reference a symbol that can resolve to 0:
   `lv2-prx.S` defines weak `module_start` / `module_stop` (returning
   `SYS_PRX_RESIDENT`) so both entries always exist.
3. Management-record NIDs: `module_start 0xBC9A0086`,
   `module_stop 0xAB779874`, `module_exit 0x3AB9A95E`,
   `module_prologue 0x0D10FD3F`, `module_epilogue 0x330F7005`. v1 emits
   the first two. There is no `module_info` NID.
4. `sys_prx_start_module` is two-phase: the kernel publishes exports and
   returns the entry to the guest wrapper, which calls it and reports the
   result; a non-zero `module_start` return unloads the module. Exports
   become visible at start, not at load.

### 8.4 Relocation segment (`0x700000A4`)

24-byte big-endian records, count = `p_filesz / 24`:

```
offset  size  field
0x00    8     offset        site = seg[index_addr].vaddr + offset
0x08    2     0
0x0a    1     index_value   segment whose runtime base is added; 0xFF = none
0x0b    1     index_addr    segment being patched
0x0c    4     type          1 ADDR32, 4 ADDR16_LO, 5 ADDR16_HI, 6 ADDR16_HA,
                            10 REL24, 11 REL14, 38 ADDR64, 44 REL64, 57 ADDR16_LO_DS
0x10    8     ptr           addend (S + A minus the chosen segment's link-time vaddr)
```

Normative rules:

1. `ptr` carries the plain value; the loader performs the HA carry, the
   `>> 2` for DS/branch forms and the PC-relative subtraction.
2. `offset < align(seg[index_addr].memsz, 0x100)` or the load aborts.
3. `prx-gen` drops relocations that are load-invariant with a single
   segment (`REL*`, `TOC16*`, `GOT16*`, `SECTOFF*`, `R_PPC64_NONE`) and
   refuses any absolute form outside the nine accepted types.
4. `.TOC.` legitimately resolves outside the segment (`.got + 0x8000`);
   it is relocated against segment 0, not frozen as an absolute value.

### 8.5 Link recipe

```
gcc -specs=$PS3DK/ppu/lib/lv2-prx.specs -nostartfiles -Wl,-q \
    -Wl,--no-warn-rwx-segments -Wl,-T,$PS3DK/ppu/lib/lv2-prx.ld \
    $PS3DK/ppu/lib/lv2-prx-crt.o <objects> <libs>
prx-gen build <elf> -o <mod>.prx --name <name> --version <M.m>
```

The specs file removes the executable-only `-T lv2.ld` that
`LINK_START_LV2_SPEC` injects unconditionally; passing a second `-T`
instead accumulates both scripts and is not conformant. `lv2-prx.ld`
must not merge `.rela.*` input sections into `.rela.dyn` - `prx-gen`
needs one `.rela.<section>` per patched section and rejects a merged
table.
