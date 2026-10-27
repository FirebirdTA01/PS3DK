# Samples

Minimal programs that exercise the toolchain, PSL1GHT, and reference-the reference SDK
ABI conformance.  One concept per sample; big examples go elsewhere.

Samples are grouped by subsystem.  New subsystems (audio, pad, network,
video, spurs, …) will get their own subdirectory when the first sample
for them lands.

## Layout

```
samples/
├── toolchain/     toolchain + ABI validation tests
├── gcm/           RSX / cellGcm graphics (libgcm + libgcm_cmd + librsx)
├── sysutil/       sysutil callbacks, dialogs, save/game/system data
├── network/       networking module load + API validation tests
├── lv2/           lv2 syscall primitives (threading, synchronisation)
├── util/          self-contained PPU utility libraries (hash)
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
| `hello-ppu-backfill` | Batch link test — anchors one symbol from each of five stub archives (subdisplay / music / music_decode / music_export / imejp) so FNID + sceResident entries land in the ELF | **green** + RPCS3 runtime-verified |

### PSGL (`toolchain/hello-psgl-*`, `toolchain/hello-psglu`)

OpenGL ES 1.1-style rendering through `libPSGL` / `libPSGLU` on top of GCM.

The 13 samples with a 900-frame render loop (blend, cmdbuf, ffp-lit-bootstrap,
ffp-quad, ffp-shaderlib, glsl-object, msaa, rotating-quad, shader-interface,
spudraw, spudraw-triangle, textured-quad, triangle) share
`toolchain/psgl-common/psgl_sample_quit.h`:

- The viewport comes from `psglGetRenderBufferDimensions`, so it matches the
  display mode (hello-psgl-msaa queries the size itself).
- A sysutil callback is polled before every frame. *Quit* from the XMB ends
  the loop before the next draw and the sample prints `quit`. A failed
  registration or poll prints `fail` and exits nonzero. Otherwise it prints
  `done` after 900 frames.

`hello-psgl-clear` (60 frames) and `hello-psgl-cg-passthrough` (4 frames)
keep their short fixed loops; the other samples do not render.

Shaders reach a sample in one of four ways:

- **`ps3_add_cg_shader_rsxcgc`**: `.vcg` / `.fcg` compiled by
  `rsx-cg-compiler` at build time and embedded with `bin2s`.
- **Fixed GLSL templates**: `tools/psgl/glsl_subset_to_cg.py` checks the
  GLSL file for the tokens of one known vertex shape and one known fragment
  shape and emits the matching fixed Cg template; it does not parse or
  validate GLSL. `rsx-cg-compiler --emit-container` then builds the
  containers.
- **Fixed-function library**: `rsx-cg-compiler --psgl-ffp-library` builds a
  shader library from `state.txt`, loaded with `psglLoadShaderLibrary`. The
  sample needs no host C compiler or `psgl-shader-builder`; configure refuses
  an `rsx-cg-compiler` that lacks the mode (use the one from the same SDK).
- **Built-in**: the fixed-function samples use the runtime's own programs.

`cgCreateProgram` cannot load a program from memory yet, and
`psglLoadShaderLibrary` takes a path, so samples that load embedded shaders
first write them to `/dev_hdd0/tmp`.

| Sample | Validates |
|---|---|
| `hello-psgl-link` | `<PSGL/psgl.h>` compiles and links against `libPSGL` + `libPSGLU` (compile + link gate) |
| `hello-psgl-clear` | Device + context bring-up, display-sized viewport, clear and swap |
| `hello-psgl-triangle` | Cg vertex + fragment program through `cgGL`, VBO, one triangle |
| `hello-psgl-blend` | Two fragment programs, alpha blending over an opaque triangle |
| `hello-psgl-msaa` | `GL_MULTISAMPLING_2X_DIAGONAL_CENTERED_SCE` device and framebuffer mode |
| `hello-psgl-textured-quad` | Texture upload and a Cg fragment program that samples it |
| `hello-psgl-shader-interface` | Cg program parameter interface through `cgGL`, VBO-fed draw |
| `hello-psgl-cg-passthrough` | A hand-built `CgBinaryProgram` through the Cg runtime |
| `hello-psgl-glsl-object` | GLSL sources mapped to fixed Cg templates at build time |
| `hello-psgl-ffp-quad` | Fixed-function textured quad: texture upload, VBO with texcoords, orthographic projection |
| `hello-psgl-rotating-quad` | Fixed-function matrix stack animation |
| `hello-psgl-ffp-lit-bootstrap` | Fixed-function lighting: normals, one light, the built-in lit programs |
| `hello-psgl-ffp-shaderlib` | `--psgl-ffp-library` shader library + `psglLoadShaderLibrary`, textured quad |
| `hello-psgl-vbo` | Buffer objects: create, size query, sub-data update, unmap, delete (no rendering) |
| `hello-psgl-cmdbuf` | Command-buffer record, call and return |
| `hello-psgl-spuhole` | Command-buffer hole sizing and alignment (no SPU) |
| `hello-psgl-spudraw` | SPU fills a command-buffer hole with NOPs; PPU replays it |
| `hello-psgl-spudraw-triangle` | SPU fills a hole with an indexed draw via `libspuPSGL` |
| `hello-psgl-fog-state` | Fog state set / query round trip (no rendering) |
| `hello-psgl-light-state` | Light, material and light-model state round trip (no rendering) |
| `hello-psglu` | `gluOrtho2Df`, `gluPerspectivef`, `gluLookAtf`, strings, `gluScaleImage` |

## gcm/

RSX / cellGcm graphics.  Incremental: start at "clear the framebuffer",
grow toward shaders and full-pipeline samples.

[`hello-ppu-gcm-frame-pacing`](gcm/hello-ppu-gcm-frame-pacing/README.md)
demonstrates VSYNC, HSYNC and application-paced VSYNC with three finite
120-frame phases. Its text summaries report flip callbacks and interval
statistics; screenshots identify the phase. These measurements do not count
hardware dropped frames. See its README for the bounded waits, XMB exit and
the published v0.26.0 FIFO-sizing caveat.

| Sample | Validates | Status |
|---|---|---|
| `hello-ppu-rsx-clear` | RSX bring-up via PSL1GHT's native `rsx*` API; cycles 6 clear colors × 60 frames (~6 s), exits clean.  No shaders | **green** + RPCS3 runtime-verified |
| `hello-ppu-cellgcm-clear` | Same flow as `rsx-clear` but spelled with `cellGcm*` identifiers wherever a forwarder exists in `<cell/gcm.h>` / `<cell/gcm/gcm_command_c.h>`.  Bit-identical RSX command-stream bytes | **green** + RPCS3 runtime-verified |
| `hello-ppu-cellgcm-cursor` | `cellGcmInitCursor` / `SetCursor*` / `UpdateCursor` — allocates a 32×32 BGRA cursor at `CELL_GCM_CURSOR_ALIGN_OFFSET`, animates a Lissajous pattern for ~6 s | **green** + RPCS3 API-verified (HW cursor overlay not rendered by RPCS3 — separate scanout-time overlay emulator doesn't draw) |
| `hello-ppu-cellgcm-triangle` | Basic GCM flip_immediate: vertex + fragment shaders in `shaders/*.{vcg,fcg}` compiled with our `rsx-cg-compiler` (`ps3_add_cg_shader_rsxcgc`; the legacy `cgcomp` path is kept commented in its CMakeLists.txt), embedded with `bin2s`, rendered as a coloured triangle | **green** (RPCS3 runtime test pending) |
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
| `hello-ppu-sysutil-queries` | License area, Remote Play status and volume, boot game info, HOME export path and paired Bluetooth devices through the new sysutil headers; loads and unloads each system module | **green** + RPCS3 runtime-verified |
| `hello-ppu-l10n` | `cellSysmoduleLoadModule(CELL_SYSMODULE_L10N)` + `eucjp2sjis()` — 165-export libl10n scaling test for the nidgen-archive pipeline | **green** + RPCS3 runtime-verified (all four EUC-JP→SJIS conversions match) |

## lv2/

lv2 syscall-layer primitives: threading, synchronisation, event flags,
timers.  These samples use the reference SDK-name sync compat headers
(`<sys/synchronization.h>`, `<sys/ppu_thread.h>`, `<sys/time_util.h>`,
`<sys/lv2_syscall.h>`) over our own / PSL1GHT's lv2 syscall surface.
The `hello-event-flag-spu` sample additionally pulls in our
`libsputhread.a` (SPU-side lv2 syscall wrappers) and `libc_stub.a`
(working `spu_printf_*` event-queue surface).

| Sample | Validates | Status |
|---|---|---|
| `hello-ppu-event-flag` | Master + 5 worker PPU threads coordinate via two `sys_event_flag_*` flags; every worker prints "succeeded my job" and exits cleanly | **green** + RPCS3 runtime-verified |
| `hello-event-flag-spu` | Full PPU + SPU sync round-trip.  Master + 5 PPU workers coordinate via `sys_event_flag_*`, then master spawns one SPU thread (linked against `libsputhread.a`) which takes commands via SNR1/SNR2 and uses `sys_event_flag_set_bit_impatient` to notify the master.  SPU thread also emits `spu_printf("SPU Worker finished my job\n")` which the PPU surfaces via our `libc_stub.a`-resident server thread (see `sdk/libc_stub_extras/src/spu_printf.c` for the dual-path impl: real-HW-canonical via `spu_thread_printf` + RPCS3-friendly direct LS read) | **green** + RPCS3 runtime-verified |
| `hello-ppu-tls` | PPU `__thread` local-exec TLS for scalar and aggregate objects; prints `TLS_OK` when the loader-initialised main-thread TLS image is visible at runtime | **green** + RPCS3 runtime-verified |
| `hello-ppu-pthread` | POSIX pthread shim over Lv-2 primitives: static mutex/cond/once initialisers, recursive mutexes, `pthread_mutex_trylock`, thread-specific data, create/join, self/equal, and condition signalling | **green** + RPCS3 runtime-verified |
| `hello-sdata` | Developer SDATA files made by `make_sdata` at build time (current format, both older layouts, 1 KB blocks, compressed layout) are written to `/dev_hdd0/tmp` and read back through `cellFsSdataOpen` in odd-sized pieces; prints `HELLO_SDATA DONE passed=5 of 5` | **green** + RPCS3 runtime-verified |
| `hello-raw-spu` | A raw SPU created, loaded and run through its problem-state registers with the `<sys/raw_spu.h>` names: `sys_raw_spu_create` / `image_load`, `SPU_In_MBox`, `SPU_NPC`, `SPU_RunCntl`, `SPU_Out_MBox` and `SPU_Status` via `sys_raw_spu_mmio_*`, the local store via `LS_BASE_ADDR`; prints `HELLO_RAW_SPU OK` | **green** + RPCS3 runtime-verified |

## audio/

cellAudio mixer + cellAudioOut configuration / discovery.

| Sample | Validates | Status |
|---|---|---|
| `hello-ppu-audio` | cellAudio Init / PortOpen / PortStart / Stop / NotifyEventQueue / AddData chain; plays a chip-tune melody via direct ring fills against `libaudio_stub.a` | **green** + RPCS3 runtime-verified |
| `hello-ppu-audio-out` | cellAudioOut surface in `<cell/cell_audio_out.h>`: GetNumberOfDevice / GetDeviceInfo / GetState / GetConfiguration via `libsysutil_audio_out_stub.a`, then a 1 s 440 Hz sine tone through cellAudio so both the discovery API and the mixer downstream get exercised in the same boot | **green** + RPCS3 runtime-verified (nDevice=0 is an RPCS3 HLE quirk; FNIDs resolve and tone plays) |

## util/

Self-contained PPU utility libraries.

| Sample | Validates | Status |
|---|---|---|
| `hello-ppu-hash` | `<cell/hash/*.h>`: MD5, SHA-1, SHA-224/256/384/512 one-call digests against the published test vectors, the block API fed in uneven pieces, the refusals, and 4 MB throughput per algorithm | **green** + RPCS3 runtime-verified (42/42) |

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
SPU-side ELF against `libspurs_task.a` (our independent SPU task
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

Samples build with **CMake**.  Each sample is a standalone project:

```bash
source ../../../scripts/env.sh     # exports PS3DEV, PS3DK
cd samples/toolchain/hello-ppu-c++17
cmake -S . -B cmake-build \
      -DCMAKE_TOOLCHAIN_FILE=../../../cmake/ps3-ppu-toolchain.cmake \
      -G Ninja
cmake --build cmake-build
```

Final artefacts land at the sample directory next to `CMakeLists.txt`;
`cmake-build/` holds intermediates (the unstripped CMake target binary,
object files, generated bin2s sources, compiled shaders, etc.).

Each sample produces three artefacts:

| Artefact | Target | How to launch |
|---|---|---|
| `<sample>.elf` | Stripped PPU ELF | `ps3load` / GDB against RPCS3's gdbstub / `rpcs3 <sample>.elf` |
| `<sample>.self` | CEX-signed SELF | `rpcs3 --no-gui <sample>.self` |
| `<sample>.fake.self` | Fake-signed SELF | `ps3load` over LAN / XMB install on CFW hardware |

**RPCS3 (preferred for quick iteration):**
```bash
rpcs3 --no-gui <sample>.self
```
The `.self` boots directly — no install step.  Stdout goes to the
RPCS3 terminal window (and `~/.cache/rpcs3/TTY.log`).

**Jailbroken hardware via `ps3load` (fastest turnaround on real HW):**
```bash
PS3LOADIP=<console IP> ps3load <sample>.fake.self
```

**Jailbroken hardware via XMB Package Installer (persistent install):**
`.pkg` generation is wired into the CMake build: add
`ps3_add_pkg(<target> CONTENTID <id>)` alongside `ps3_add_self` and the
`.pkg` is produced as part of a normal build.  To drive the
`make_self_npdrm` + `sfo` + `pkg` + `package_finalize` chain by hand
instead, see the *Manual / direct toolchain invocation* section in the
top-level `README.md` for the exact command sequence.

## Retest matrix after major rebuilds

After any rebuild of binutils / GCC+newlib / PSL1GHT, the three canonical
green samples must still link and (where possible) run:

1. `toolchain/hello-ppu-c++17` — toolchain validation test.
2. `toolchain/hello-ppu-abi-check` — ABI oracle.
3. PSL1GHT's own `sample/` tree — verifies downstream homebrew still
   compiles against the modernised headers.

A rebuild isn't "done" until all three still pass.

## Contributing a new sample

1. Pick (or create) the right subsystem directory.
2. Create `<subsystem>/<name>/` with `CMakeLists.txt` and `source/main.c`
   (or `.cpp`).
3. The `CMakeLists.txt` boilerplate is short — `cmake_minimum_required`,
   `project(<name> C [ASM])` (add `ASM` if `bin2s` / shader / SPU
   embedding is involved), `include(ps3-self)`, `add_executable`,
   `target_link_libraries`, then `ps3_add_self(<target>)`.  Copy from a
   neighbouring sample with similar requirements.  For embedded SPU
   code, call `ps3_add_spu_image(<target> NAME <name> SOURCES ...)`
   instead of maintaining a `spu/` subdir.
4. Add a row in the relevant table above with what it validates.
5. Keep samples minimal — one concept per sample.  Big examples go
   elsewhere.
6. If the sample exercises an ABI boundary, document the hex-dump shape
   it expects so future regressions are visible in `objdump -s`.
