# Samples

Minimal programs that exercise the toolchain, PSL1GHT, and reference-cell-SDK
ABI conformance.  One concept per sample; big examples go elsewhere.

Samples are grouped by subsystem.  New subsystems (audio, pad, network,
video, spurs, …) will get their own subdirectory when the first sample
for them lands.

## Layout

```
samples/
├── toolchain/     toolchain + ABI smoke tests
├── gcm/           RSX / cellGcm graphics (libgcm + libgcm_cmd + librsx)
├── sysutil/       sysutil callbacks, dialogs, save/game/system data
├── lv2/           lv2 syscall primitives (threading, synchronisation)
├── spu/           SPU toolchain + PPU/SPU interop
└── spurs/         libspurs sync primitives + Spurs task runtime
```

## toolchain/

Validate the toolchain, `libsysbase` lv2 glue, and .sys_proc_param ABI.
These are the "does the compiler work" tier — don't touch RSX / sysutil.

| Sample | Validates | Status |
|---|---|---|
| `hello-ppu-c++17` | PPU C++17 front- and back-end, libstdc++ link, PPU thread id | **green** |
| `hello-ppu-abi-check` | `.sys_proc_param` 36-byte firmware-3.30+ layout; malloc/calloc/realloc through libsysbase `_sbrk_r`; printf through `_write_r` | **green** |
| `hello-ppu-backfill` | Batch link smoke test — anchors one symbol from each of five stub archives (subdisplay / music / music_decode / music_export / imejp) so FNID + sceResident entries land in the ELF | **green** + RPCS3 runtime-verified |

## gcm/

RSX / cellGcm graphics.  Incremental: start at "clear the framebuffer",
grow toward shaders and full-pipeline samples.

| Sample | Validates | Status |
|---|---|---|
| `hello-ppu-rsx-clear` | RSX bring-up via PSL1GHT's native `rsx*` API; cycles 6 clear colors × 60 frames (~6 s), exits clean.  No shaders | **green** + RPCS3 runtime-verified |
| `hello-ppu-cellgcm-clear` | Same flow as `rsx-clear` but spelled with `cellGcm*` identifiers wherever a forwarder exists in `<cell/gcm.h>` / `<cell/gcm/gcm_command_c.h>`.  Bit-identical RSX command-stream bytes | **green** + RPCS3 runtime-verified |
| `hello-ppu-cellgcm-cursor` | `cellGcmInitCursor` / `SetCursor*` / `UpdateCursor` — allocates a 32×32 BGRA cursor at `CELL_GCM_CURSOR_ALIGN_OFFSET`, animates a Lissajous pattern for ~6 s | **green** + RPCS3 API-verified (HW cursor overlay not rendered by RPCS3 — separate scanout-time overlay emulator doesn't draw) |
| `hello-ppu-cellgcm-triangle` | Basic GCM flip_immediate: vertex + fragment shaders in `shaders/*.{vcg,fcg}` compiled via PSL1GHT `cgcomp` (default) or our own `rsx-cg-compiler` with `USE_RSX_CG_COMPILER=1`, embedded with `bin2s`, rendered as a coloured triangle | **green** (RPCS3 runtime test pending) |
| `hello-ppu-cellgcm-flip` | 4-buffer flip_immediate demonstration.  No shaders / no geometry — animation is a scissor-rect stripe sweeping across a dark background so the flip-queue mechanics (VBlank handler → SetFlipImmediate, flip handler → IDLE release) are the only thing driving what's on screen | **green** (RPCS3 runtime test pending) |
| `hello-ppu-cellgcm-cube` | Rotating 3D cube.  8 vertices + 36-index buffer, per-PPU perspective × view × rotation matrix math, depth-test on so back-facing geometry occludes naturally | **green** (RPCS3 runtime test pending) |
| `hello-ppu-cellgcm-quad` | Textured quad.  4-vertex triangle strip, 64×64 procedural checker texture uploaded to RSX local memory, `cellGcmSetTexture` + sampler filter / address / control setup, fragment program with a `sampler2D` uniform bound to TEXUNIT0 | **green** (RPCS3 runtime test pending) |
| `hello-ppu-cellgcm-sysmenu` | Spinning triangle that pauses when the XMB system menu opens: the PPU freezes the rotation angle and overlays an animated "PAUSED" text strip (drifts + pulses via `cellDbgFont`); CLOSE resumes normal rendering, EXITGAME exits cleanly | **green** (RPCS3 runtime test pending) |
| `hello-ppu-dbgfont` | `cellDbgFont*` on-screen text overlay demo over a clear-loop background: multi-size / multi-colour text grid covering the whole screen, animated pulsing + drifting strings, per-frame counter | **green** (RPCS3 runtime test pending) |

## sysutil/

cellSysutil callback surface, modal dialogs, system parameters, savedata,
gamedata, screenshot, ap, l10n.  These all go through the PSL1GHT runtime
today with cellSDK-named forwarders; samples that need a
non-PSL1GHT-backed library pull one of the nidgen-built stub archives.

| Sample | Validates | Status |
|---|---|---|
| `hello-ppu-sysutil-cb` | `cellSysutilRegisterCallback` / `Check` / `Unregister`; FNIDs 0x9d98afa0 / 0x189a74da / 0x02ff3c1b | **green** + RPCS3 runtime-verified (XMB events delivered) |
| `hello-ppu-msgdialog` | `cellMsgDialogOpen2` Yes/No dialog; FNIDs 0xf81eca25 / 0x7603d3db / 0x62b0f803 | **green** + RPCS3 runtime-verified |
| `hello-ppu-sysparam` | `cellSysutilGetSystemParam{Int,String}` — language / nickname / date / timezone round-trip; FNIDs 0x40e895d3 / 0x938013a0 | **green** + RPCS3 runtime-verified |
| `hello-ppu-savedata` | `cellSaveDataAutoSave2` — full stat + file callback dance writing a 256-byte payload; FNID 0x8b7ed64b | **green** + RPCS3 runtime-verified (PARAM.SFO + SAVEDATA.BIN land) |
| `hello-ppu-gamedata` | `cellGameBootCheck` + `ContentPermit` + `GetParam{Int,String}`; FNIDs 0xf52639ea / 0x70acec67 / 0x3a5d726a / 0xb7a45caf | **green** + RPCS3 runtime-verified |
| `hello-ppu-screenshot` | `cellScreenShot{Enable,Disable,SetParameter}` backed purely by `libsysutil_screenshot_stub.a`; FNIDs 0x9e33ab8f / 0xfc6f4e74 / 0xd3ad63e4 | **green** + RPCS3 runtime-verified (loader resolved the SPRX cleanly) |
| `hello-ppu-ap` | `cellSysutilApGetRequiredMemSize` + `Off` over `libsysutil_ap_stub.a`; FNIDs 0x9e67e0dd / 0x3343824c / 0x90c2bb19 | **green** + RPCS3 runtime-verified (returns expected 1 MiB container size) |
| `hello-ppu-l10n` | `cellSysmoduleLoadModule(CELL_SYSMODULE_L10N)` + `eucjp2sjis()` — 165-export libl10n scaling test for the nidgen-archive pipeline | **green** + RPCS3 runtime-verified (all four EUC-JP→SJIS conversions match) |

## lv2/

lv2 syscall-layer primitives: threading, synchronisation, event flags,
timers.  These samples use the cell-SDK-name sync compat headers
(`<sys/synchronization.h>`, `<sys/ppu_thread.h>`, `<sys/time_util.h>`)
over PSL1GHT's existing lv2 syscall surface.  No SPRX modules required.

| Sample | Validates | Status |
|---|---|---|
| `hello-ppu-event-flag` | Master + 5 worker PPU threads coordinate via two `sys_event_flag_*` flags; every worker prints "succeeded my job" and exits cleanly | **green** + RPCS3 runtime-verified |

## spu/

SPU toolchain, SPU intrinsics, and PPU/SPU interop (DMA, mailboxes,
signals).  Runs spu-elf-gcc on the SPU side with combined PPU/SPU build
machinery.

| Sample | Validates | Status |
|---|---|---|
| `hello-spu` | SPU toolchain + `spu_*`/`mfc_*` intrinsics + DMA write back to PPU | **green** |

## spurs/

libspurs sync primitives + Spurs task runtime.  PPU side goes through
`<cell/spurs.h>` (Spurs / Spurs2 attribute + scheduler bring-up,
Taskset class-0 and class-1, event-flag / queue / semaphore / barrier
sub-surfaces, task + workload APIs) linking against
`libspurs_stub.a`.  The Spurs-task samples additionally link their
SPU-side ELF against `libspurs_task.a` (our clean-room SPU task
runtime: `__spurs_task_start` crt + `cellSpursExit` /
`cellSpursTaskExit` / `cellSpursGet{TaskId,TasksetAddress}` /
`cellSpursYield` / `cellSpursTaskPoll` dispatching through the
SPRX-owned control block at LS 0x2fb0) using the
`$PS3DK/spu/ldscripts/spurs_task.ld` linker script that places task
code at `CELL_SPURS_TASK_TOP` (LS 0x3000).

| Sample | Validates | Status |
|---|---|---|
| `hello-ppu-spurs-core` | Spurs2 4-SPU bring-up + SPU thread-id enumeration + finalize via the `cell::Spurs::SpursAttribute` / `Spurs2` C++ class wrappers | **green** + RPCS3 runtime-verified |
| `hello-ppu-spurs-event-flag` | `cellSpursEventFlag*` IWL variant end-to-end: set + wait + GetTasksetAddress | **green** + RPCS3 runtime-verified |
| `hello-ppu-spurs-queue` | `cellSpursQueue*` IWL variant: init, size/depth/entry-size/direction queries, TryPop-busy return path | **green** + RPCS3 runtime-verified |
| `hello-ppu-spurs-semaphore` | `cellSpursSemaphore*` IWL variant: init + GetTasksetAddress | **green** + RPCS3 runtime-verified |
| `hello-spurs-task` | Full PPU + SPU round-trip.  PPU spawns an embedded SPU task via `cellSpursCreateTask` on a class-0 taskset.  SPU task receives a PPU-visible flag EA in `argTask.u64[0]`, DMAs `0xC0FFEE` via `mfc_put`, exits via `cellSpursTaskExit(0)`; PPU observes the flag, Shutdown+Join, finalize | **green** + RPCS3 runtime-verified |

## Ideas for growth

Planned additions follow the same per-subsystem pattern.  Representative
near-term candidates:

- **`audio/`** — `cellAudio` port open / mix bus / stop
- **`pad/`** — `cellPad` init + poll + actuator (force-feedback)
- **`net/`** — `cellNetCtl` link check, HTTPS GET via mbedTLS + libcurl
- **`video/`** — `cellVideoOut` mode detection + resolution switch

Each new subsystem gets its own subdirectory; new samples within an
existing subsystem drop into the relevant directory.

## What `toolchain/hello-ppu-abi-check` checks

It is the ABI oracle: if it regresses, our output drifted from the
reference loader's expectations.

- **`.sys_proc_param` section size** is 0x24 (36 bytes), 8-byte aligned.
- **Magic** `0x13bcc5f6`.
- **Version** `0x00330000` (firmware 3.30+ layout).  PSL1GHT's historical
  `0x00009000` is rejected by the retail PS3 loader on firmware 3.30+;
  RPCS3 is permissive enough to let either through, so passing on RPCS3
  is necessary but not sufficient.
- **Struct** has 9 fields including the trailing `crash_dump_param_addr`.
- **Allocator** — 8 × 32 KiB blocks through `malloc → _malloc_r → sbrk →
  _sbrk_r (libsysbase) → __syscalls.sbrk_r`, zero-verified `calloc`,
  `realloc` grows.
- **stdio** — every `printf` routes through `_write_r` →
  `__syscalls.write_r` (installed by PSL1GHT crt1).

Run under RPCS3 with `--no-gui`; a passing run ends with `result: PASS`.

Hex dump under `powerpc64-ps3-elf-objdump -s -j .sys_proc_param
hello-ppu-abi-check.elf`:

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

## Building

```bash
source ../../../scripts/env.sh     # exports PS3DEV, PSL1GHT
cd samples/toolchain/hello-ppu-c++17
make
```

Each sample produces three artefacts:

| Artefact | Target | How to launch |
|---|---|---|
| `<sample>.elf` | Raw PPU ELF | `ps3load` / GDB against RPCS3's gdbstub / `rpcs3 <sample>.elf` |
| `<sample>.self` | CEX-signed SELF | `rpcs3 --no-gui <sample>.self` |
| `<sample>.fake.self` | Fake-signed SELF | `make run` (streams over LAN to `ps3load` on CFW hardware) |

**RPCS3 (preferred for quick iteration):**
```bash
rpcs3 --no-gui <sample>.self
```
The `.self` boots directly — no install step.  Stdout goes to the
RPCS3 terminal window (and `~/.cache/rpcs3/TTY.log`).

**Jailbroken hardware via `ps3load` (fastest turnaround on real HW):**
```bash
export PS3LOADIP=<console IP>
make run
```

**Jailbroken hardware via XMB Package Installer (persistent install):**
```bash
make <sample>.pkg    # produces <sample>.pkg + <sample>.gnpdrm.pkg
```
Copy the `.pkg` to a USB stick at `/PS3/GAME/`, boot the console, and
install via *Game → Package Installer*.  The app shows up in XMB under
the sample's `TITLE` (see the Makefile) and can be launched like a
normal game.  Uninstall via *Game → Game → Delete*.

`.pkg` isn't a default `make` target because the `.self` already covers
both RPCS3 and dev-iteration flows; only build it when you want a
persistent retail-style install.

## Retest matrix after major rebuilds

After any rebuild of binutils / GCC+newlib / PSL1GHT, the three canonical
green samples must still link and (where possible) run:

1. `toolchain/hello-ppu-c++17` — toolchain smoke test.
2. `toolchain/hello-ppu-abi-check` — ABI oracle.
3. PSL1GHT's own `sample/` tree — verifies downstream homebrew still
   compiles against the modernised headers.

A rebuild isn't "done" until all three still pass.

## Contributing a new sample

1. Pick (or create) the right subsystem directory.
2. Create `<subsystem>/<name>/` with `Makefile` and `source/main.c`
   (or `.cpp`).
3. Use `include $(PSL1GHT)/ppu_rules` for PPU-only; add a `spu/` subdir
   with `include $(PSL1GHT)/spu_rules` for SPU code.
4. Add a row in the relevant table above with what it validates.
5. Keep samples minimal — one concept per sample.  Big examples go
   elsewhere.
6. If the sample exercises an ABI boundary, document the hex-dump shape
   it expects so future regressions are visible in `objdump -s`.
