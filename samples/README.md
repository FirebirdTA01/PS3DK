# Samples

Minimal programs that exercise the toolchain, PSL1GHT, and Sony-ABI conformance.
Each sample has one concept; big examples go elsewhere.

## Current samples

| Sample | Validates | Depends on | Status |
|---|---|---|---|
| `hello-ppu-c++17/` | PPU C++17 front- and back-end, libstdc++ link, PPU thread id | Phase 1 complete | **green** |
| `hello-ppu-sony-abi/` | `.sys_proc_param` section layout (36-byte Sony 3.30+), malloc/calloc/realloc through libsysbase `_sbrk_r`, printf through `_write_r` | Phase 1 + Phase 3 milestone 1 | **green** |
| `hello-ppu-sysutil-cb/` | Sony-named `cellSysutilRegisterCallback`/`Check`/`Unregister` through PSL1GHT runtime — first Phase 6 backfill sample, imports Sony FNIDs 0x9d98afa0/0x189a74da/0x02ff3c1b | Phase 3 + patch psl1ght/0011 | **green** + **RPCS3 runtime-verified** (XMB events delivered) |
| `hello-ppu-msgdialog/` | Sony-named `cellMsgDialogOpen2` Yes/No dialog through PSL1GHT runtime — second Phase 6 backfill sample, imports Sony FNIDs 0xf81eca25 / 0x7603d3db / 0x62b0f803 | Phase 3 + patch psl1ght/0012 | **green** + **RPCS3 runtime-verified** (dialog rendered, YES button routed back) |
| `hello-ppu-sysparam/` | Sony-named `cellSysutilGetSystemParam{Int,String}` — third Phase 6 backfill sample.  Prints language / nickname / date format / timezone / etc.  Imports Sony FNIDs 0x40e895d3 / 0x938013a0 | Phase 3 + patch psl1ght/0013 | **green** + **RPCS3 runtime-verified** (numeric + string params round-trip) |
| `hello-ppu-savedata/` | Sony-SDK-source-compat `cellSaveDataAutoSave2` — fourth Phase 6 backfill sample.  Full stat + file callback dance writing a 256-byte payload into an autosave slot.  Imports Sony FNID 0x8b7ed64b | Phase 3 + patch psl1ght/0014 | **green** + **RPCS3 runtime-verified** (PARAM.SFO + SAVEDATA.BIN both land; 256 bytes written via 2-callback protocol) |
| `hello-ppu-gamedata/` | Sony-SDK-source-compat `cellGameBootCheck` + `ContentPermit` + `GetParam{Int,String}` — fifth Phase 6 backfill sample.  Probes how the running app was launched and reports its PARAM.SFO.  Imports Sony FNIDs 0xf52639ea / 0x70acec67 / 0x3a5d726a / 0xb7a45caf | Phase 3 + patch psl1ght/0015 | **green** + **RPCS3 runtime-verified** (all calls return 0; Boot-SELF run returns empty context as expected — launch via `.pkg` install to see populated fields) |
| `hello-ppu-screenshot/` | Sony-SDK-source-compat `cellScreenShotEnable` / `Disable` / `SetParameter` — first Phase 6.5 sample.  Validates the nidgen-archive pipeline end-to-end: `libsysutil_screenshot_stub.a` (slots + FNIDs + `.lib.stub` + sceResident + `.sceStub.text` trampolines + `.opd` descriptors) is the only library backing — no PSL1GHT SPRX wrapper.  Imports Sony FNIDs 0x9e33ab8f / 0xfc6f4e74 / 0xd3ad63e4 (overlay 0x7a9c2243 declared, not called) | Phase 3 + patch psl1ght/0016 + `scripts/build-cell-stub-archives.sh` | **green** + **RPCS3 runtime-verified** (all three calls return 0x00000000; loader resolved cellScreenShotUtility SPRX, patched stub slots, trampolines ran clean) |
| `hello-ppu-ap/` | Sony-SDK-source-compat `cellSysutilApGetRequiredMemSize` + `Off` — second Phase 6.5 sample.  Second consumer of the nidgen-archive pipeline; backs onto `libsysutil_ap_stub.a`.  Imports Sony FNIDs 0x9e67e0dd / 0x90c2bb19 (and 0x3343824c via whole-archive include) | Phase 3 + patch psl1ght/0017 + `scripts/build-cell-stub-archives.sh` | **green** + **RPCS3 runtime-verified** (GetRequiredMemSize returns exactly 0x100000 = 1 MiB matching the header constant; Off returns 0x00000000) |
| `hello-ppu-l10n/` | Port of Sony's `samples/sdk/l10n/eucjp2sjis.c` — first Sony-SDK-source port.  Calls `cellSysmoduleLoadModule(CELL_SYSMODULE_L10N)` then `eucjp2sjis()` for a few hardcoded EUC-JP code points, then unloads.  First nidgen-archive consumer above ~10 exports (libl10n_stub: 165 exports, archive scales fine).  Imports Sony FNIDs 0x32267a31 / 0x112a5ee9 + libl10n's `eucjp2sjis` FNID | Phase 3 + patches psl1ght/0018 + 0019 + `scripts/build-cell-stub-archives.sh` | **green** + **RPCS3 runtime-verified** (all four EUC-JP→SJIS conversions match expected: a4a2→82a0, a4a4→82a2, c6fc→93fa, cbdc→967b — proves SPRX-resolved Sony code is executing) |
| `hello-spu/` | SPU toolchain + intrinsics + DMA back to PPU, combined PPU/SPU build | Phase 2a complete | **green** |
| `ppu-spu-dma/` (TBD) | Full DMA mailbox exchange patterns | Phase 2a complete | not yet |
| `rsx-triangle-vs/` (TBD) | Existing PSL1GHT vertex shader pipeline still works | Phase 3 Beta | not yet |
| `rsx-textured-quad-fs/` (TBD) | New NV40-FP fragment shader assembler | Phase 3 M5 | not yet |
| `net-http-get/` (TBD) | Networking + mbedTLS HTTPS | Phase 4 + Phase 3 networking | not yet |

## What `hello-ppu-sony-abi` checks

It is the ABI oracle: if it regresses, our output drifted from Sony's expectations.

- **`.sys_proc_param` section size** is 0x24 (36 bytes), 8-byte aligned.
- **Magic** `0x13bcc5f6`.
- **Version** `0x00330000` (firmware 3.30+ layout).  PSL1GHT's historical
  version `0x00009000` is rejected by the retail PS3 loader on firmware
  3.30+; RPCS3 is permissive enough to let either through, so passing on
  RPCS3 is necessary but not sufficient.
- **Struct** has 9 fields including the trailing `crash_dump_param_addr`.
- **Allocator** — writes and reads 8 × 32 KiB blocks via `malloc` → free,
  zero-verified `calloc`, `realloc` grows.  End-to-end path:
  `malloc → _malloc_r → sbrk → _sbrk_r (libsysbase) → __syscalls.sbrk_r`
  (installed by PSL1GHT crt1).
- **stdio** — every `printf` routes through `_write_r` → `__syscalls.write_r`.

Run under RPCS3 with `--no-gui` to watch the stdout; a passing run ends
with `result: PASS`.

`.sys_proc_param` hex dump under `powerpc64-ps3-elf-objdump -s -j
.sys_proc_param hello-ppu-sony-abi.elf` should look like:

```
 xxxxx 00000024 13bcc5f6 00330000 ffffffff  ...$.....3......
 xxxxx 000003e9 00010000 00100000 00000000  ................
 xxxxx XXXXXXXX                             ....
```

Fields in order: size=36, magic, version=VERSION_330, sdk_version=UNKNOWN,
prio=1001, stacksize=0x10000, malloc_pagesize=1M, ppc_seg=DEFAULT,
crash_dump_param_addr=low 32 bits of `__sys_process_crash_dump_param`'s
OPD address (the sample defines the callback; omitting it leaves this
word at 0, which is Sony's "no callback" semantics).

Task #7 delivered via patch `patches/psl1ght/0009`: the
`SYS_PROCESS_PARAM` macro now emits the whole struct through inline asm
so the trailing field can be a `R_PPC64_ADDR32` relocation against the
weak `__sys_process_crash_dump_param` symbol.  User code just defines
the callback (or doesn't).

## Building

```bash
source ../scripts/env.sh   # exports PS3DEV, PSL1GHT
cd hello-ppu-c++17
make
```

Artifacts:
- `<sample>.elf` — the PPU ELF
- `<sample>.self` — signed for RPCS3 / fake-signed for jailbroken hardware
- `build/` — intermediates

## Running

`make` produces three artifacts per sample; pick the one that matches your
target.

| Artifact | Target | How to launch |
|---|---|---|
| `<sample>.elf` | Raw PPU ELF — for `ps3load` or GDB debugging | `make run` (ps3load) or `rpcs3 <sample>.elf` |
| `<sample>.self` | CEX-signed SELF — boots on RPCS3 or signed hardware | `rpcs3 --no-gui <sample>.self` |
| `<sample>.fake.self` | Fake-signed SELF — for jailbroken/CFW hardware via `ps3load` | `make run` (streams over LAN) |

**RPCS3 (preferred for quick iteration):**
```bash
rpcs3 --no-gui <sample>.self
```
The `.self` boots directly — no install step.  Stdout goes to the RPCS3
terminal window.

**Jailbroken hardware via `ps3load` (fastest turnaround on real HW):**
```bash
export PS3LOADIP=<console IP>
make run   # streams <sample>.fake.self to the running ps3load on the console
```

**Jailbroken hardware via XMB Package Installer (persistent install):**
```bash
make <sample>.pkg   # produces <sample>.pkg + <sample>.gnpdrm.pkg
```
Copy the `.pkg` to a USB stick at `/PS3/GAME/` or similar, boot the
console, and install via *Game → Package Installer*.  The app shows up in
XMB under the sample's `TITLE` (see the Makefile) and can be launched
like a normal game.  Uninstall via *Game → Game → Delete*.

`.pkg` isn't a default `make` target because the `.self` already covers
both RPCS3 and dev-iteration flows; only build it when you want a
persistent retail-style install.

## Retest matrix after major rebuilds

After any rebuild of binutils / GCC+newlib / PSL1GHT, all three green
samples must still link and (where possible) run:

1. `hello-ppu-c++17` — our own toolchain smoke test (C++17 + PSL1GHT runtime).
2. `hello-ppu-sony-abi` — Sony-ABI oracle.  A regression here means we
   drifted from Sony's loader expectations even if RPCS3 still loads the
   binary.
3. PSL1GHT's own `sample/` tree (once built) — verifies downstream
   homebrew still compiles against the modernised headers.

A rebuild isn't "done" until all three still pass.

## Contributing a new sample

1. Create `samples/<name>/` with `Makefile`, `source/main.c` (or `.cpp`).
2. Use `include $(PSL1GHT)/ppu_rules` for PPU-only; add `spu/` subdir with
   `include $(PSL1GHT)/spu_rules` for SPU code.
3. Add a row in the table above with what it validates and which phase
   must be complete before it can build.
4. Keep samples minimal — one concept per sample.  Big examples go
   elsewhere.
5. If the sample exercises a Sony-ABI boundary, document the hex-dump
   shape it expects so future regressions are visible in `objdump -s`.
