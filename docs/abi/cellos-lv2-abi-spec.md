# CellOS Lv-2 PPU ABI - normative specification

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
| `e_flags`        | `0x00000000`                        |

Upstream readelf prints the OS/ABI byte as `<unknown: 66>` - this is expected.
Our fork of binutils (when added) SHOULD render it as `CellOS Lv-2`.

> **Note (2026-04-23 revision):** `e_flags` was previously documented as
> `0x01000000`. Cross-checking decrypted CEX SELFs built with the reference
> SDK (ftpd, cellftp, mount_hdd, setmonitor, db_backup, and the BD / PS1 /
> PS2 / PSP emulator shells) shows every binary carrying `e_flags = 0x0`.
> Our `0001-elf64-ppc-tag-cellos-lv2-headers.patch` hardcoded the non-zero
> value based on an earlier (incorrect) reading of the spec; that path is
> being reverted in a follow-up.

---

## 2. Compact function-descriptor format (`.opd`)

> **Status - achieved.** The toolchain now emits 8-byte compact function
> descriptors natively via GCC's `-mps3-opd-compact` flag (default for the
> `powerpc64-ps3-elf` target). Every `.opd` entry is exactly 8 bytes and is
> laid out as:

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
   Reference-SDK `crt1.o` emits this value hardcoded in every shipping
   CEX binary surveyed (ftpd, cellftp, mount_hdd, setmonitor, etc.).
   Zero here causes the loader - both on hardware and RPCS3 - to
   stall inside `_sys_prx_register_module` before the module's entry
   point is invoked. The two bytes encode a major/minor pair
   (`0x01` / `0x01`) but the meaning is not yet spec-clear; the
   normative requirement is the literal value, not the interpretation.

---

## 4. Pointer and addressing model — ELF64 + ILP32 hybrid

> **Status — implemented 2026-05-07 on `feature/mlp64-abi`** (commits
> 3db8ce9 + a08c775 + 305d133 + 22605c3 + 16a628c + 1e076ed).
> The PPU GCC target now defaults to **ILP32 with a 64-bit ELF
> wrapper** (Pmode = SImode, ELFCLASS64, EM_PPC64).  Pointers are
> 32-bit on the wire, registers are 64-bit, the ELF header still
> reports ELF64.  Multilib `-mlp64` opt-in flips Pmode to DImode for
> the legacy 64-bit-pointer path.

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
   patched GCC so its `.toc` sections inherit the 4-byte layout —
   any pre-patch build is silently ABI-stale.
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

> **Status — implemented 2026-05-07** (commit 16a628c).  Frame-less
> trampolines previously used caller_sp+24 / +40 for LR/TOC saves.
> ELFv1 reserves caller_sp+16 as the *callee*'s LR-save slot, which
> the SPRX function (called via `bctrl`) overwrites on entry.  The
> framed shape protects the trampoline's own TOC save inside its
> 64-byte frame, where the SPRX cannot reach.

Normative trampoline body (per export, ILP32 hybrid):

```asm
__<name>:
    mflr   r0
    stw    r0, 16(r1)         ; LR -> caller's reserved callee-LR slot
    stwu   r1, -64(r1)         ; allocate 64-byte frame
    stw    r2, 24(r1)          ; TOC -> our own frame slot, beyond SPRX reach
    lis    r12, <name>_stub@ha
    lwz    r12, <name>_stub@l(r12)
    lwz    r0, 0(r12)          ; SPRX entry EA (compact OPD slot 0)
    lwz    r2, 4(r12)          ; SPRX TOC (compact OPD slot 4)
    mtctr  r0
    bctrl                       ; call SPRX
    lwz    r2, 24(r1)           ; restore caller TOC from our frame
    addi   r1, r1, 64           ; pop our frame
    lwz    r0, 16(r1)           ; restore caller LR from caller-reserved slot
    mtlr   r0
    blr
```

Normative rules:

1. **Caller LR save** at `caller_sp+16` (stored BEFORE the `stwu`).
   This is the ELFv1 callee-reserved slot; saving in our caller's
   reserved area prevents the SPRX function we `bctrl` into from
   overwriting it (it writes at `our_sp+16`, which lies inside our
   own 64-byte frame).
2. **Caller TOC save** at `our_sp+24` (stored AFTER the `stwu`).
   Inside our frame the SPRX function cannot reach it.
3. **Frame size 64 bytes** — gives the SPRX callee sufficient
   parameter-save area (its `sp+24+` is our `sp+24+`, which is below
   our TOC slot at `+24`... actually our frame extends from `our_sp`
   upward 64 bytes, the SPRX callee's stwu pushes another frame
   below that).  >8-arg SPRX exports are NOT currently emitted by
   nidgen (audited 2026-05-07: zero >8-arg or variadic exports
   across all 29 nidgen DBs), so the +64 SP shift is safe.  Audit
   re-required if a >8-arg export ever lands.
4. **Compact OPD descriptor read** at `lwz r0, 0(r12); lwz r2,
   4(r12)` — the `.data.sceFStub` slot was written by the loader
   with the resolved 8-byte compact OPD address.
5. Saves and loads use 4-byte `stw` / `lwz`, never 8-byte
   `std` / `ld`; pointers are 32-bit under the ILP32 hybrid.
6. **Single `.lib.stub` header per imported library** —
   liblv2.a/sprx.o (legacy PSL1GHT-style hand-rolled stubs) MUST NOT
   coexist with nidgen-emitted liblv2_stub.a in the link.  Both
   emit a `.lib.stub` header naming `sysPrxForUser`; with both in
   the link the loader resolves duplicate sysPrxForUser imports and
   the trampoline shape from each archive may differ.  As of commit
   1e076ed nidgen owns sysPrxForUser canonical Sony names plus
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
