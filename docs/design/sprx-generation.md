# SPRX / PRX module generation — design

Board task: `t_107ce1f6`. Follow-ups this design sequences: `t_fe7c705a` (export round-trip), `t_4bfdccc9` (CachyOS reference read — shares the host/mount).
Companion facts memo (local-only, public-source citations with RPCS3 file:line): `docs/local/sprx-format-facts.md` (claude_PS3DK, 2026-08-29). **The memo is the byte-level spec; this document is the design and the contract between the pieces.**
Status: **v2, 2026-08-29 02:00** — implementation started overnight on the director's instruction ("working (s)prx build option + RPCS3-tested sample by morning"). Tags: `[C]` confirmed from public source or our tree (link-tested where stated), `[R]` confirm on the CachyOS 475.001 read (read-only, never copied — `reference/REFERENCE_POLICY.md`).

## 0. Decisions

| Question | Decision | Why |
|---|---|---|
| Route | **(b) link a normal ET_EXEC image at base 0 with `-Wl,-q` (`--emit-relocs`), post-process into a PRX.** Not PIE/DYN. | Our binutils/GCC/crt/ld-script stack is EXEC-shaped and verified against reference executables; `-q` leaves that stack untouched and hands us exactly the relocation list the loader needs. PIE would re-open the compact-OPD/TOC contract (spec §2, §4) for no gain. xerpi's preferred route in PSL1GHT #67; the VitaSDK technique (`vita-elf-create`, on PATH here — technique reference only). |
| Segments | **One `PT_LOAD`, flags RWX, base 0, `p_offset = 0`.** | Every relocation then has `index_addr == index_value == 0` and `ptr` = the link-time address — one class of conversion bugs removed. RX/RW split is a follow-up after the reference module layout is read `[R]`. |
| Export tables | **In source, not synthesised.** The crt emits the module-info skeleton and the management record; the module's own `exports.S` emits its named-library record through `<sys/prx_module.h>` macros, with `num_func` computed by assembler label arithmetic. | Nothing is patched post-link on the export side, so the silent-skip failure of a wrong `attributes` word (memo §1.5/§5.2) cannot happen by construction. `nidgen entgen` (YAML → `exports.S`) is the follow-up that automates this; the format is unchanged by it. |
| Tool | **New Rust crate `tools/prx-gen/`** (workspace member; ships via the host-tools glob; must be listed in `cmake/ps3-required-artifacts.txt`). `tools/sprx-linker` untouched (executables only; it refuses an ELF with `.sceStub.text` and no `.sys_proc_prx_param`, which is every PRX). | The tool only does what `ld` cannot: convert `.rela.*` to the `0x700000A4` segment, rewrite `EI_OSABI`/`e_type`/`e_entry`, write the module-info file offset into `PHDR[0].p_paddr`, write name/version/attributes, and redo the `.lib.stub` import-count fixup for imports. |
| Module identity | Written by `prx-gen` post-link (`--name/--version/--attributes`); crt block is zeros there. | Keeps the crt generic; no per-module crt compile. |
| Entry points | crt provides **weak** `module_start`/`module_stop` returning `SYS_PRX_RESIDENT` with real compact OPDs, and emits the management record naming both. | An address of 0 in a management record makes the loader **throw** (memo §3: RPCS3 evaluates every special's descriptor eagerly, `PPUModule.cpp:795`, `PPUAnalyser.h:242`). Weak-undefined references would resolve to 0; weak *definitions* never do. |
| Signing | `prx-gen` emits a raw ELF `.prx`. Two container routes: **fake-signed** `.sprx` via `fself` (SELF `se_flags 0x8000`, `self_type 4`; RPCS3 skips key decryption for such containers — `Crypto/unself.cpp` — and CFW hardware loads them like `.fake.self`), and **real-signed** `.sprx` via `make_sprx` (geohot `make_self.c` with `-DSPRX`: 3.55 app keys, authid `0x1070000052000001`, `s_flags=7`, `i_apptype 4`, no section table), whose loadability depends on the loader's key table (RPCS3 `FindSelfKey(program_type, se_flags, sceversion)`; a miss is `CELL_PRX_ERROR_UNSUPPORTED_PRX_TYPE 0x80011148`). | RPCS3 loads the unsigned `.prx` when the booted main executable is a raw `.elf` (memo §4.3) — format bugs and signing bugs are debugged separately, in that order. **Outcome (2026-08-29 17:44):** the `make_sprx` real-signed container fails in RPCS3's `FindSelfKey` (`Failed to decrypt SELF metadata info` → `0x80011148`) before any ELF parsing; the `fself` fake-signed `.sprx` loads and runs first try (`PRX_OK` + `PRX_IMPORT_OK` via a signed container). **Shipped behaviour (director's pairing rule, 17:50): `ps3_add_prx(... SIGN)` emits BOTH `<target>.sprx` (`make_sprx`, real-signed) and `<target>.fake.sprx` (`fself`), exactly as `ps3_add_self` emits `.self` + `.fake.self`; a `.self` loads the `.sprx`, a `.fake.self` loads the `.fake.sprx` — no mode flag.** Real signing is solvable (the keys are public since fail0verflow 2010); the open item is only that RPCS3's key table lacks `make_sprx`'s (program-type, `s_flags=7`, sceversion) triple — hardware acceptance untested (director's PS3). |
| TLS | **Refused by `prx-gen` in v1** (non-empty `.tdata`/`.tbss`). | The loader accepts only `PT_LOAD` and `0x700000A4` (no `PT_TLS`), and `num_tlsvar != 0` is an error path in RPCS3 (memo §1.6). |

## 1. What already existed (verified in-tree; do not re-derive)

| Piece | Where | Relevance |
|---|---|---|
| `.sys_proc_prx_param` + `.lib.ent.top`/`.lib.stub.top` prefixes | `runtime/lv2/crt/lv2-sprx.S`; spec §3 | Executable-only block. A PRX has none; the module-info block (§2.1) replaces it. The prefix convention (`range start + 4`) is kept. |
| Import surface (44-byte `.lib.stub` record, `.rodata.sceFNID/.sceResident`, `.data.sceFStub.<lib>`, `.sceStub.text`, compact 8-byte `.opd`) | `tools/nidgen/src/stubgen.rs`; `section-reference.md` §4 | **Reused unchanged**; `imports_start/end` point at the range. `stubgen.rs:105` writes the export count into the `attributes` field (benign on imports, fatal if copied to exports — memo §5.2); our export writer is separate. |
| Post-link import-count fixup | `tools/sprx-linker/sprx-linker.c:253–288` | Re-implemented inside `prx-gen` (offset 6 of each `.lib.stub` record = FNID-run length ÷ 4). |
| Compact OPD, trampoline shape, relocation-class contract | spec §2, §5, §5.1 | Importer trampolines read `{addr32, toc32}` from the slot the loader fills with the **exporter's OPD address** ⇒ a function export's `addrs[]` entry is simply the function symbol (its value *is* the descriptor). Verified in the link test: `.rela.rodata.sceExportAddr` = `ADDR32 module_start / module_stop / prx_add` → `.opd` entries. `[C]` |
| `sysPrxLoadModule/StartModule/StopModule/UnloadModule/GetModuleIdByName` | `ppu/include/sys/prx.h`, `lv2/prx.h` (installed and in the v0.10.720 extract) | `sysPrxStartModule` is an **import of `sysPrxForUser` NID `0x9f18429d`** (`src/ps3dev/PSL1GHT/ppu/sprx/liblv2/exports.h:28`), i.e. the userland wrapper that performs the two-phase start (memo §3.1) — not a raw syscall 481. `[C]` on the NID; `[R]`/RPCS3-run on the two-phase behaviour. |
| GCC link specs | `patches/ppu/gcc-12.x/0002-…:303` `LINK_START_LV2_SPEC "-T lv2.ld%s"`, unconditional | Must be overridden for a PRX link (§3.1); an accumulating second `-T` is **not** safe. |
| Rust ELF patterns | `tools/spu-elf-to-ppu-obj/src/{ppu_write.rs,spu_elf.rs}` | Reading side only; the PRX writer is a hand-rolled big-endian ELF64 emitter (custom `p_type`/`p_paddr`). |
| Packaging law | `cmake/ps3-required-artifacts.txt`; `scripts/build-host-tools-windows.sh:421` (`cargo build --workspace`, every `*.exe` staged) | `prx-gen.exe` and `make_sprx.exe` need `host_bin` rows; the crt pieces need `sdk_core` rows; otherwise the packager refuses to zip. |

## 2. Format facts (from the memo; the memo is the citation of record)

### 2.1 Module-info block — 52 bytes `[C]`

`u16 attributes @0x00; u8 version[2] @0x02; char name[28] @0x04; u32 toc @0x20; u32 exports_start @0x24; u32 exports_end @0x28; u32 imports_start @0x2C; u32 imports_end @0x30`. **Found via `PHDR[0].p_paddr` = the block's file offset** (read at `seg[0].vaddr + p_paddr − p_offset`); no magic, no NID, no note. `p_paddr == 0` ⇒ "PRX library info not found" ⇒ no exports. The relocation segment is applied **before** the block is read, so `toc` and the four range pointers must carry `ADDR32` relocation entries. Handoff correction: `sys_prx_module_info_t` is the *output* of `_sys_prx_get_module_info`, not the in-file block.

### 2.2 Relocation segment `[C]`

`p_type = 0x700000A4`; only `PT_LOAD` and this type are accepted. 24-byte entries: `u64 offset @0; u16 unk0 @8; u8 index_value @0xA; u8 index_addr @0xB; u32 type @0xC; u64 ptr @0x10`. Site = `seg[index_addr].vaddr + offset`; value = `(index_value == 0xFF ? 0 : seg[index_value].vaddr) + ptr`. Hard bound `offset < align(seg[index_addr].size, 0x100)` or the loader throws. Accepted types, exhaustive: `1 ADDR32, 4 ADDR16_LO, 5 ADDR16_HI, 6 ADDR16_HA, 10 REL24, 11 REL14, 38 ADDR64, 44 REL64, 57 ADDR16_LO_DS`; the loader encodes (HA carry, `>>2`, PC-relative subtraction) — `ptr` is the raw value, never pre-encoded.

### 2.3 Library record — 44 bytes, same shape for `.lib.ent` and `.lib.stub` `[C]`

`u8 size(0x2c) @0; u8 unk0; u16 version @2; u16 attributes @4; u16 num_func @6; u16 num_var @8; u16 num_tlsvar @0xA; u8 info_hash; u8 info_tlshash; u16 unk1; u32 name @0x10; u32 nids @0x14; u32 addrs @0x18; u32 vnids; u32 vstubs; u32 unk4; u32 unk5`. Walk stride = `size` (0 ⇒ 44). **Export side:** variables are appended to the *same* `nids`/`addrs` arrays after the functions; `vnids`/`vstubs` are only logged. `attributes`: `0x0001` = named library (register + publish); `0x8000` with `0x0001` clear = the nameless management record; **neither ⇒ silently skipped** (no log line). Function `addrs[]` entries are compact OPD addresses. Every address in a record is dereferenced eagerly at load; an address outside the module throws.

### 2.4 Entry points `[C]`

Ordinary function exports of the management record: `module_start 0xBC9A0086`, `module_stop 0xAB779874`, `module_exit 0x3AB9A95E`, `module_prologue 0x0D10FD3F`, `module_epilogue 0x330F7005`. There is no `module_info` *function* the loader reads (earlier draft was wrong about a NID lookup); however the firmware's own `liblv2` logs `** Special: &[module_info]` — reference modules export a `module_info` **variable** in the management record that RPCS3 ignores `[R: purpose/layout on CachyOS]`. `sys_prx_start_module` is two-phase: cmd 1 publishes exports and returns the entry to the *guest* caller, the guest calls it, cmd 2 reports the result; non-zero from `module_start` ⇒ immediate unload. Exports become visible at **start**, not load. The guest-side wrapper is `sysPrxStartModule` (`sysPrxForUser` NID `0x9f18429d`; HLE in RPCS3 `Emu/Cell/Modules/sys_prx_.cpp:90`), which performs both phases in one call — importers do not hand-roll the protocol. Three wrapper rules `[C]`: the `modres` pointer must be non-NULL (`CELL_EINVAL` otherwise, before anything happens); the wrapper prefers `entry2` (= `module_prologue`, returned only when `pOpt->size != 0x20`) over `entry` (= `module_start`) — a module that exports `module_prologue` changes the call shape to `prologue(entry, args, argp)` and `module_start` is no longer called directly, which is why v1 does not emit it; and `module_start`'s return becomes phase-2 `res`, so it must be `SYS_PRX_RESIDENT` (0).

### 2.5 ELF identity — hard gates `[C]`

`ELFCLASS64`, big-endian, `EI_OSABI = 0x66`, `e_type = 0xFFA4`, `e_machine = 21`, `e_ehsize 64`, `e_phentsize 56`, `e_shentsize 64`. `EI_OSABI` and `e_type` are post-link rewrites (a stock `ld` gives SysV/`ET_EXEC`). `e_entry` is never read by the loader (`[R]` for real Lv-2 — we write 0). Section headers optional. Unsigned `.prx` loads only when the booted main executable path ends in `.elf` (memo §4.3).

### 2.6 `[R]` items for the CachyOS read

Segment count/flags/section names of a shipped `.sprx`; whether `p_paddr − p_offset` is ever nonzero; reloc types beyond the nine; `unk*`/`info_hash` fields; `e_entry`; module-info `attributes`/`version` values; TLS semantics; import-binding timing for user modules on real Lv-2; multi-library modules.

## 3. The pieces and their contract

```
 exports.S (prx_module.h macros) ──as──▶ exports.o ─┐
 sources ───────────────────────────────▶ objects ──┤
 lv2-prx-crt.o (module-info skeleton, prefixes, weak module_start/stop, management record)
 lib*_stub.a (imports; unchanged) ──────────────────┤
                                                    ▼
   gcc -specs=lv2-prx.specs -nostartfiles -Wl,-q -Wl,-T,lv2-prx.ld ... ──▶ <mod>.elf  (ET_EXEC @0, one RWX LOAD, .rela.* per section)
                                                    │
                          prx-gen build --name --version [--attributes]
                                                    ▼
                                                <mod>.prx  (0x66 / 0xFFA4 / e_entry 0 / p_paddr / 0x700000A4 segment)
                                                    │
                                          make_sprx (optional)
                                                    ▼
                                                <mod>.sprx
 exports.yml ──nidgen archive──▶ lib<mod>_stub.a   (importers link this; existing path)
 <mod>.prx  ──prx-gen exports──▶ yml  ══ exports.yml   (t_fe7c705a, follow-up)
```

### 3.1 Link step `[C — link-tested 2026-08-29 01:45]`

* **Flags** (set by `ps3_add_prx`): `-specs=$PS3DK/ppu/lib/lv2-prx.specs -nostartfiles -Wl,-q -Wl,-T,$PS3DK/ppu/lib/lv2-prx.ld $PS3DK/ppu/lib/lv2-prx-crt.o <objs> <libs> -Wl,--no-warn-rwx-segments`. The specs file body is exactly `*link_start_lv2:` / `%{mprx-nothing:}` — verified with `-###` to remove the injected `-T lv2.ld` while keeping the `-lsysbase -lc -lrt -llv2` group. `-nostartfiles` drops `lv2-crti/crt0/sprx.o` and `crtbegin/crtend` (static constructors do not run in a v1 module — non-goal).
* **`runtime/lv2/crt/lv2-prx.ld`**: `lv2.ld` minus `ENTRY`, minus `hdr_param/hdr_prx/hdr_tls`, minus the `.rel.dyn`/`.rela.dyn` merge blocks (they would destroy the per-target `sh_info` `-q` relocations carry); `PHDRS { hdr_load PT_LOAD FILEHDR PHDRS FLAGS(7) }`; `. = SIZEOF_HEADERS`; `.rodata.sceModuleInfo` first; new export sections `.rodata.sceExportName/.sceExportNID/.sceExportAddr` KEEP'd before `.lib.ent.*`; everything for imports, compact `.opd ALIGN(4)`, `.got/.toc`, `__lv2_toc_bias` verbatim from `lv2.ld`; `.tdata/.tbss` placed (not discarded) so `prx-gen` can refuse them.
* **`runtime/lv2/crt/lv2-prx.S`**: module-info skeleton (symbol `__sys_prx_module_info`, section `.rodata.sceModuleInfo`; `toc = .TOC.`, ranges = `__libentstart+4 / __libentend / __libstubstart+4 / __libstubend`); the two prefix words; weak `module_start`/`module_stop` (`li 3,0; blr`, compact OPD in the exact shape GCC 12.4 emits); the management record (`0x2c, version 1, attributes 0x8000, num_func 2`, nids `0xBC9A0086, 0xAB779874`, addrs → the two OPDs).
* **`sdk/include/sys/prx_module.h`**: C side — `SYS_PRX_RESIDENT/NO_RESIDENT`, NID and attribute constants, prototypes `int module_start(size_t, void*)` / `module_stop`; asm side — `PRX_LIBRARY_BEGIN(tag, "name")`, `PRX_EXPORT_FUNC(sym, nid)`, `PRX_LIBRARY_END(tag)` emitting name string, parallel NID/addr arrays and one full 44-byte `.lib.ent` record with `attributes 0x0001` and `num_func = (nids_end − nids)/4`. Listed in `sdk/Makefile` `HEADERS`.
* **Install**: `scripts/build-runtime-lv2.sh` builds `lv2-prx-crt.o` for ILP32 and LP64 and installs `.o/.ld/.specs` next to `lv2.ld` in both trees; manifest rows `sdk_core ppu/lib/{lv2-prx-crt.o,lv2-prx.ld,lv2-prx.specs}` + `ppu/lib/lp64/lv2-prx-crt.o`.
* **Link-test result** (scratch module: `prx_add`, a data table, a function pointer, a strong `module_start`): one `PT_LOAD` `off 0 vaddr 0 filesz 0x234 memsz 0x238 RWE`; `.rodata.sceModuleInfo` at file offset 0x78; `.rela.rodata.sceModuleInfo` = exactly five `ADDR32` (`.TOC.`, the four range symbols) — **the `.TOC.` relocation survives `-q`**; `.rela.opd` = two `ADDR32` per entry; whole-file reloc set `ADDR32 ×22, REL32 ×2 (.eh_frame), TOC16_HA ×2, TOC16_LO ×2`; `.lib.ent` bytes: management `2c00 0001 8000 0002 … name 0`, named `2c00 0001 0001 0001 … name → "hello_prx"`. `.TOC.` resolves to `0x8200` (= `__lv2_got_start + 0x8000`), a value outside the segment — normal r2 bias; the loader bounds-checks sites, not values.

### 3.2 `prx-gen` (`tools/prx-gen/`, Rust, MIT) — claude_PS3DK

`prx-gen build <in.elf> -o <out.prx> --name N --version M.m [--attributes 0x..]`; `prx-gen check <prx>`; `prx-gen exports <prx> -o <yml>` (follow-up).

1. **Validate**: ET_EXEC PPC64 BE; exactly one `PT_LOAD`; `.tdata/.tbss` empty; `.rodata.sceModuleInfo` present inside `PHDR[0]`; per-section `.rela.*` (fail on a merged `.rela.dyn`); every reloc type accepted or droppable.
2. **Import-count fixup** on `.lib.stub` records only (sprx-linker algorithm).
3. **Relocation conversion**: site → `index_addr`, `offset = r_offset − seg.vaddr`; value `V = S + A`; `SHN_ABS`/undefined ⇒ `index_value 0xFF`, else segment of V; **drop** same-segment `REL24/REL14/REL32/REL64`, all `TOC16*`, `R_PPC64_NONE`, `TLSGD`/`TOCSAVE` markers; **convert** `ADDR32→1, ADDR16_LO→4, HI→5, HA→6, ADDR64→38, ADDR16_LO_DS→57`; anything else ⇒ error naming reloc/section/symbol. Raw values in `ptr`.
4. **Module-info guard**: fail if fewer than five converted relocations land inside the block; synthesise an `ADDR32` for a non-zero unrelocated field only with a warning.
5. **Identity**: write name (≤27, error otherwise), version, attributes into the block.
6. **Write**: ELF64 BE header (`0x66`, `0xFFA4`, `e_entry 0`, `e_phnum 2`), `PT_LOAD` copied with `p_paddr` = module-info file offset, `0x700000A4` segment last; no section headers in v1.
7. **`check`**: re-open the output, derive the block from `p_paddr`, walk both ranges by stride, verify `attributes ∈ {0x0001 set, 0x8000-only}`, all `nids/addrs/name` pointers inside the segment, management record carries `0xBC9A0086`; decode every reloc at a fake base and assert the five block pointers land inside their ranges.

### 3.3 CMake surface (`cmake/ps3-self.cmake`) — codex

```cmake
ps3_add_prx(<target> NAME <≤27> VERSION <M.m> [ATTRIBUTES <hex>] [SIGN])
ps3_prx_stub_library(<lib-target> EXPORTS <exports.yml>)    # lib<name>_stub.a via nidgen archive
```
`ps3_add_prx` = `add_executable` + the §3.1 link flags + POST_BUILD `strip → prx-gen build → [make_sprx]`; tool probe mirrors the existing `find_program(… NO_DEFAULT_PATH)` block and FATAL_ERRORs with the same "run build-runtime-lv2.sh / extract the zip" wording when a `sdk_core` crt piece is missing. Outputs next to the CMakeLists like `ps3_add_self`; the importer sample loads `/app_home/<name>.prx`.

### 3.4 Round-trip (follow-up, `t_fe7c705a`)

`nidgen entgen <exports.yml> -o exports.S` (YAML → the same macros) and `prx-gen exports` (PRX → YAML). Requires a `kind: function | variable | tlsvar` field on `Export` (`db.rs:41`, `serde(default)` = `function`, existing YAMLs unchanged); records are functions-first so `num_func/num_var` and the shared arrays line up (§2.3).

## 4. Deliverables

| # | Deliverable | Path | Owner | State (02:00) |
|---|---|---|---|---|
| 1 | Design doc | `docs/design/sprx-generation.md` | Fable | this |
| 2 | Facts memo | `docs/local/sprx-format-facts.md` | claude_PS3DK | done |
| 3 | crt + ld + specs + install | `runtime/lv2/crt/lv2-prx.{S,ld,specs}`, `scripts/build-runtime-lv2.sh` | Fable | done, link-tested |
| 4 | Export macros header | `sdk/include/sys/prx_module.h`, `sdk/Makefile` HEADERS | Fable | done |
| 5 | `prx-gen` crate + manifest row | `tools/prx-gen/`, `tools/Cargo.toml`, `tools/Cargo.lock`, `cmake/ps3-required-artifacts.txt` | claude_PS3DK | done; 13 unit tests on a synthetic BE ELF (U0) incl. `.TOC.`-outside-segment, unencoded addends, attributes-0 rejection, re-run refusal |
| 6 | `make_sprx` host tool + manifest row | `scripts/build-host-tools-windows.sh`, manifest | codex | in progress |
| 7 | CMake surface | `cmake/ps3-self.cmake` | codex | in progress |
| 8 | Samples | `samples/lv2/hello-sprx-export/`, `hello-sprx-import/` | codex | in progress |
| 9 | Spec update (§8 "Lv-2 module layout"), `section-reference.md` §3 record layout, `abi-verify` PRX mode | `docs/abi/…`, `tools/abi-verify` | Fable | after the RPCS3 pass |
| 10 | `nidgen entgen` + `kind` field; `prx-gen exports` | `tools/nidgen`, `tools/prx-gen` | claude_PS3DK | morning |

Cargo is available in WSL Ubuntu only (`/home/firebirdta01/.cargo/bin/cargo`, targets `x86_64-pc-windows-gnu` + linux); the dev loop builds `prx-gen.exe` there and copies it to `$PS3DK/bin`. Nothing is committed until the director's `approved:`; every owner keeps an explicit-path file list.

## 5. Test plan

Acceptance runs on the **desktop release RPCS3 only** (`scripts/rpcs3-claim.ps1` / `rpcs3-release.ps1`, one instance, wide fatal grep, emulator exit status separate from guest TTY, logs preserved).

| Level | What | Pass criterion |
|---|---|---|
| U0 | `prx-gen` unit tests (each accepted reloc type; cross-segment REL24; dropped TOC16; refused TLS; name > 27; `p_paddr` re-read; module-info reloc guard) | `cargo test --workspace` green (CI `.github/workflows/ci.yml:55`) |
| U1 | `prx-gen check` on `hello-sprx-export.prx`; later `prx-gen exports` == `exports.yml` | clean / diff empty |
| **A1** | **`hello-sprx-import` booted as a raw `.elf`** loads the unsigned `hello-sprx-export.prx` via `sysPrxLoadModule("/app_home/…")`, `sysPrxStartModule(argp=&table)`; `module_start` fills `table` with `prx_add`'s pointer; the importer calls it and prints `PRX_OK <sentinel>`; `sysPrxStopModule` + `sysPrxUnloadModule` return 0 | TTY sentinel, exit 0, no `EMULATION_FROZEN`; `RPCS3.log` free of "Unknown segment type", "Illegal/Unknown type", "PRX library info not found", "get_ref(): Failure" |
| A2 | Importer links `libhello_prx_stub.a` (nidgen archive from the same NIDs) and calls `prx_add` through the trampoline after `StartModule` | sentinel via the trampoline; records RPCS3's binding timing (§2.6) |
| A3 | A module that *imports* (`printf` from inside `module_start`) — proves `imports_start/end` + the in-module count fixup | TTY line from the module |
| A4 | signed module loaded from a `.fake.self`-booted importer: (a) `fself`-produced fake-signed `.sprx` — **PASS 2026-08-29** (exit_0, 0 fatal, both call paths); (b) `make_sprx` real-signed `.sprx` — **FAIL** `0x80011148` (RPCS3 key table has no entry for its program-type/`s_flags=7`/sceversion) | paired re-boot 17:50 PASS with `.sprx`, `.fake.sprx` and `.prx` all staged and the `.fake.self` importer picking `.fake.sprx`; (b) remains a hardware/keyset question |
| H1 | Hardware (director's jailbroken PS3): A1/A2 on the `.sprx` | deferred; first real-Lv-2 data point |
| S1 | Fresh-extract sweep, raw + post-setup | 198/198 post-setup once the two samples exist |

## 6. Open items

| Item | Owner |
|---|---|
| CachyOS read (§2.6) — combined with `t_4bfdccc9` | director + lead |
| Whether `make_sprx`/signing is part of the first milestone or only the unsigned RPCS3 path | director (default: both attempted; A1 first) |
| RX/RW segment split, TLS, variable exports, multi-library modules, `sys_prx_register_library` from user code, NPDRM modules, static constructors in modules | follow-ups (§0 non-goals) |
