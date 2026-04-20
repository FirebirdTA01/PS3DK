# Samples

Minimal programs that exercise the toolchain, PSL1GHT, and reference-cell-SDK
ABI conformance.  Each sample has one concept; big examples go elsewhere.

## Current samples

| Sample | Validates | Status |
|---|---|---|
| `hello-ppu-c++17/` | PPU C++17 front- and back-end, libstdc++ link, PPU thread id | **green** |
| `hello-ppu-abi-check/` | `.sys_proc_param` section layout (36-byte firmware-3.30+), malloc/calloc/realloc through libsysbase `_sbrk_r`, printf through `_write_r` | **green** |
| `hello-ppu-sysutil-cb/` | `cellSysutilRegisterCallback` / `Check` / `Unregister` over the PSL1GHT runtime; imports FNIDs 0x9d98afa0 / 0x189a74da / 0x02ff3c1b | **green** + RPCS3 runtime-verified (XMB events delivered) |
| `hello-ppu-msgdialog/` | `cellMsgDialogOpen2` Yes/No dialog over the PSL1GHT runtime; imports FNIDs 0xf81eca25 / 0x7603d3db / 0x62b0f803 | **green** + RPCS3 runtime-verified (dialog rendered, YES button routed back) |
| `hello-ppu-sysparam/` | `cellSysutilGetSystemParam{Int,String}` — prints language / nickname / date format / timezone etc.  Imports FNIDs 0x40e895d3 / 0x938013a0 | **green** + RPCS3 runtime-verified (numeric + string params round-trip) |
| `hello-ppu-savedata/` | `cellSaveDataAutoSave2` — full stat + file callback dance writing a 256-byte payload into an autosave slot.  Imports FNID 0x8b7ed64b | **green** + RPCS3 runtime-verified (PARAM.SFO + SAVEDATA.BIN both land; 256 bytes written via 2-callback protocol) |
| `hello-ppu-gamedata/` | `cellGameBootCheck` + `ContentPermit` + `GetParam{Int,String}` — probes how the running app was launched and reports its PARAM.SFO.  Imports FNIDs 0xf52639ea / 0x70acec67 / 0x3a5d726a / 0xb7a45caf | **green** + RPCS3 runtime-verified (all calls return 0; Boot-SELF run returns empty context as expected — launch via `.pkg` install to see populated fields) |
| `hello-ppu-screenshot/` | `cellScreenShotEnable` / `Disable` / `SetParameter` — validates the nidgen-archive pipeline end-to-end: `libsysutil_screenshot_stub.a` is the only library backing.  Imports FNIDs 0x9e33ab8f / 0xfc6f4e74 / 0xd3ad63e4 (overlay 0x7a9c2243 declared, not called) | **green** + RPCS3 runtime-verified (all three calls return 0; loader resolved cellScreenShotUtility SPRX, patched stub slots, trampolines ran clean) |
| `hello-ppu-ap/` | `cellSysutilApGetRequiredMemSize` + `Off`, backed by `libsysutil_ap_stub.a`.  Imports FNIDs 0x9e67e0dd / 0x90c2bb19 (and 0x3343824c via whole-archive include) | **green** + RPCS3 runtime-verified (GetRequiredMemSize returns exactly 0x100000 = 1 MiB; Off returns 0) |
| `hello-ppu-l10n/` | A small `eucjp2sjis` port: `cellSysmoduleLoadModule(CELL_SYSMODULE_L10N)` then `eucjp2sjis()` over a few EUC-JP code points, then unload.  Libl10n_stub has 165 exports — scales the nidgen-archive pipeline past small-size cases.  Imports FNIDs 0x32267a31 / 0x112a5ee9 + libl10n's `eucjp2sjis` FNID | **green** + RPCS3 runtime-verified (all four EUC-JP→SJIS conversions match expected) |
| `hello-ppu-backfill/` | Batch link smoke test — pulls one symbol from each of five stub archives (subdisplay / music / music_decode / music_export / imejp; 99 exports across 5 SPRX modules) into a non-call sink so the FNID + sceResident entries land in the ELF | **green** + RPCS3 runtime-verified (all 10 `.opd` addresses non-null, well-spread; process boots, prints, exits clean) |
| `hello-ppu-event-flag/` | Port of the `lv2/event_flag` reference sample (PPU half): master + 5 worker PPU threads coordinate via two `sys_event_flag_*` event flags.  Pure lv2 syscall sample, no SPRX/stub-archive needed | **green** + RPCS3 runtime-verified (master + all 5 workers exchange event-flag signals; every worker prints "succeeded my job"; threads exit cleanly) |
| `hello-ppu-rsx-clear/` | Brings up the RSX command stream and cycles a clear-color over the framebuffer using `rsxClearSurface` (no shaders).  Adapted from PSL1GHT's `cairo` sample's `rsxutil.c`.  Cycles 6 colors × 60 frames each (~6 s), exits clean.  Press START to exit early | **green** + RPCS3 runtime-verified (RSX up at 1280×720, 360 frames drawn, all 6 colors visible on screen, exited clean) |
| `hello-ppu-cellgcm-clear/` | Same flow as `hello-ppu-rsx-clear` but spelled with `cellGcm*` identifiers wherever a forwarder exists in `<cell/gcm.h>` + `<cell/gcm/gcm_command_c.h>`.  Only `rsxMemalign`/`rsxFree` stay PSL1GHT-named (no cellGcm equivalent) | **green** + RPCS3 runtime-verified (ctx populated at 0x2e0ccc, 1280×720 init, 360 frames drawn, all 6 colors visible — bit-identical visual result to rsx-clear) |
| `hello-ppu-cellgcm-cursor/` | Exercises the `cellGcmInitCursor` / `SetCursor*` / `UpdateCursor` family — PS3 RSX hardware cursor is a video overlay, no shaders.  Allocates a 32×32 BGRA cursor texture at `CELL_GCM_CURSOR_ALIGN_OFFSET` (2048-byte) alignment, animates it in a Lissajous pattern over a sweeping background for ~6 s | **green** + RPCS3 API-verified (all six cellGcm cursor calls return 0; texture allocates 2048-aligned at expected offset; 360 frames drawn clean exit).  HW cursor overlay is a separate scanout-time overlay that RPCS3 doesn't visually emulate; API contract is satisfied and would render on real PS3 hardware |
| `hello-ppu-cellgcm-triangle/` | A basic GCM flip_immediate example.  Vertex + fragment shaders in `shaders/*.{vcg,fcg}` are compiled via PSL1GHT `cgcomp` (or our own `rsx-cg-compiler` with `USE_RSX_CG_COMPILER=1`), embedded via `bin2s`, and rendered as a coloured triangle | **green** (RPCS3 runtime test pending) |
| `hello-spu/` | SPU toolchain + intrinsics + DMA back to PPU, combined PPU/SPU build | **green** |
| `ppu-spu-dma/` (TBD) | Full DMA mailbox exchange patterns | not yet |
| `rsx-triangle-vs/` (TBD) | Existing PSL1GHT vertex shader pipeline still works | not yet |
| `rsx-textured-quad-fs/` (TBD) | New NV40-FP fragment shader assembler | not yet |
| `net-http-get/` (TBD) | Networking + mbedTLS HTTPS | not yet |

## What `hello-ppu-abi-check` checks

It is the ABI oracle: if it regresses, our output drifted from the reference
loader's expectations.

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
.sys_proc_param hello-ppu-abi-check.elf` should look like:

```
 xxxxx 00000024 13bcc5f6 00330000 ffffffff  ...$.....3......
 xxxxx 000003e9 00010000 00100000 00000000  ................
 xxxxx XXXXXXXX                             ....
```

Fields in order: size=36, magic, version=VERSION_330, sdk_version=UNKNOWN,
prio=1001, stacksize=0x10000, malloc_pagesize=1M, ppc_seg=DEFAULT,
crash_dump_param_addr=low 32 bits of `__sys_process_crash_dump_param`'s
OPD address (the sample defines the callback; omitting it leaves this
word at 0, which is the "no callback" value).

The `SYS_PROCESS_PARAM` macro emits the whole struct through inline asm
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
2. `hello-ppu-abi-check` — reference-ABI oracle.  A regression here means we
   drifted from the reference loader's expectations even if RPCS3 still loads
   the binary.
3. PSL1GHT's own `sample/` tree (once built) — verifies downstream
   homebrew still compiles against the modernised headers.

A rebuild isn't "done" until all three still pass.

## Contributing a new sample

1. Create `samples/<name>/` with `Makefile`, `source/main.c` (or `.cpp`).
2. Use `include $(PSL1GHT)/ppu_rules` for PPU-only; add `spu/` subdir with
   `include $(PSL1GHT)/spu_rules` for SPU code.
3. Add a row in the table above with what it validates.
4. Keep samples minimal — one concept per sample.  Big examples go
   elsewhere.
5. If the sample exercises an ABI boundary, document the hex-dump
   shape it expects so future regressions are visible in `objdump -s`.
