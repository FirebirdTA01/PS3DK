# Samples

Minimal programs that check the toolchain, PSL1GHT, and the platform ABI.
One concept per sample; big examples go elsewhere.

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
├── sprx/          relocatable modules (PRX/SPRX) and their loaders
├── util/          self-contained PPU utility libraries (hash)
├── spu/           SPU toolchain + PPU/SPU interop
├── spurs/         libspurs sync primitives + Spurs task runtime
├── audio/         cellAudio mixer + cellAudioOut discovery
├── codec/         codec decode/encode surfaces (pngenc, jpgdec, sail, …)
├── dbgfont/       cellDbgFont on-screen text overlay
├── debug/         debugger / GDB practice sample
├── font/          system font (cellFont) rendering
├── fw/            firmware (FW) framework validation samples
├── input/         keyboard / mouse / GEM / DualShock 3 input
├── PSL1GHT/       upstream PSL1GHT sample ports
├── profiling/     profiler probes + benchmark drivers
└── vision/        camera (Eye) capture + GCM presentation
```

## input/

`hello-ppu-gem-basic` reads Move buttons, trigger and inertial records without
a camera. It includes bounded rumble, sphere colour controls and GEM/SPURS
cleanup in both ABIs. See its [README](input/hello-ppu-gem-basic/README.md)
for controls and opt-in runtime checks. Hardware IMU, rumble and sphere colour
verification is pending; emulator checks cover API/input/lifecycle only.

`ds3-bench` prints DualShock 3 buttons, sticks and pressure values and fires
200 ms rumble pulses on the small and large motors, with port selection,
pressure-mode restore and reconnect handling. See its
[README](input/ds3-bench/README.md) for controls. Hardware verification is
pending.

| Sample | Validates | Status |
|---|---|---|
| `host-tests` | native-host unit tests for the portable `ds3-bench` logic (use the SDK `cell/pad.h` for constants / structs only; no PS3 code runs) |  |

## profiling/

[`heterogeneous-workload`](profiling/heterogeneous-workload/README.md) cycles
the same checked compute batch between the PPU and up to six persistent SPE
workers, with light and heavy RSX rendering. It demonstrates manual PPU scopes,
per-SPE job telemetry and raw GPU timestamp reports on non-DEX homebrew consoles.
It currently requires the candidate profiler SDK; the raw device readings are
not yet a calibrated system timeline. Profiling can be disabled for comparison.

[`rsx-frame`](profiling/rsx-frame/README.md) submits a known draw matrix
(2 vertex x 3 fragment programs, three texture formats, depth / cull / blend
variants, an indexed draw, an instance run, labels in nested scopes) every frame,
requests an RSX frame capture from app code and exits cleanly; its checker
compares the captured frame with an expected table field by field. Hardware and
RPCS3 validation are pending.

[`spurs-job-zones`](profiling/spurs-job-zones/README.md) is a finite candidate
proof of explicit SPU zones inside job-queue jobs. Each job has a distinct lane,
uses the job context's DMA tag, and returns a lane-bound receipt after successful
writer finalization. ON/OFF controls and one/two enabled-SPU configurations are
provided. Its firmware execution and decrementer behavior still require target
validation; a successful build is not that validation.

| Sample | Validates | Status |
|---|---|---|
| `clock-calibration` | RAW observations of the SPU decrementer and the RSX timestamp, each bracketed between two PPU timebase reads (records only; no calibration claim) |  |
| `clock-calibration-w` | localization probe W / W0: a clock-probe variant (c4 path) on the shared probe harness |  |
| `ext-probe` | the first on-console SPU-lane + RSX-domain probe (design p3.3 E1) |  |
| `launch-marker` | launch-marker probe L: the smallest program on the clock probe's launch path (a single marker + timebase pair) |  |
| `live-read-probe` | live-link hardware gate 0: can an app thread read the profiler's capture file while the target is running |  |
| `overhead-bench` | the portable overhead-bench target driver (Plan A r2 probe design, A-1 to A-4 + D7) |  |
| `wait-observations` | ordinary `pthread` calls with capture-lifecycle only (no manual zones / wait hooks); 32 mutex calls + a distinct mutex, handshaked |  |

## toolchain/

Validate the toolchain, `libsysbase` lv2 glue, and .sys_proc_param ABI.
These are the "does the compiler work" tier — don't touch RSX / sysutil.

| Sample | Validates | Status |
|---|---|---|
| `hello-ppu-c++17` | PPU C++17 front- and back-end, libstdc++ link, PPU thread id | **green** |
| `hello-ppu-abi-check` | `.sys_proc_param` 36-byte firmware-3.30+ layout; malloc/calloc/realloc through libsysbase `_sbrk_r`; printf through `_write_r` | **green** |
| `hello-ppu-backfill` | Batch link test — anchors one symbol from each of five stub archives (subdisplay / music / music_decode / music_export / imejp) so FNID + sceResident entries land in the ELF | **green** + RPCS3 runtime-verified |
| `compact-opd-probe` | proof-of-concept for 8-byte compact function descriptors (call through lwz/lwz/mtctr/bctrl inline asm) |  |
| `gcm-config-abi` | empirical kernel-ABI probe for `cellGcmGetConfiguration` (the `CellGcmConfig` widener) |  |
| `hello-cellcgb-parse` | `<cell/cgb.h>` / libCGB parser smoke test (parses a CGB file; the `file io failed` path is a distinct check) |  |
| `hello-ppu-c++17-oss` | C++17 `std::ostringstream<<int` crash isolation (a minimal repro of the integer-insertion fault) |  |
| `hello-ppu-dtoa-minimal` | newlib `dtoa` (float-to-string) minimal validation: a `printf("%g")` round-trip on a small set of floats |  |
| `hello-ppu-lp64-syscall-probe` | LP64 ABI bisect: `cellGcm` / `rsxMemalign` probes in a minimal program, stepped with `-DSTEP=N` |  |
| `hello-ppu-lp64-video-out-probe` | LP64 ABI bisect: `cellVideoOut` + `cellGcm` in a minimal program, stepped with `-DSTEP=N` (N = 1 to 4) |  |
| `hello-ppu-mlp64-tagged-pointer` | packs a 16-bit type tag into the unused high 16 bits of a 64-bit pointer and recovers it via mask + shift (works under `-mlp64`, not ILP32) | builds; LP64 artefact is produced at link time but currently cannot reach `main()` at runtime (ILP32-shape CRT objects; see the sample README) |
| `hello-ppu-mlp64-types` | probe: prints `sizeof` of every width-sensitive primitive + the state of `__LP64__` / `__ILP32__` (built twice so both ABI flavours ship) | builds; LP64 artefact is produced at link time but currently cannot reach `main()` at runtime (ILP32-shape CRT objects; see the sample README) |
| `hello-ppu-mt19937` | C++ `std::mt19937` deterministic sequence: known 32-bit values + a 100-iteration check |  |
| `hello-ppu-oss-int-midflight` | snapshot the `ostringstream` internal buffer before and after `oss << 1` to locate the corruption |  |
| `hello-ppu-oss-int-nobanner` | `ostringstream<<int` variant: no `printf` banner before the ostringstream block |  |
| `hello-ppu-oss-int-noctor` | `ostringstream<<int` variant: a global `ostringstream`, constructed far from use |  |
| `hello-ppu-oss-int-only` | `ostringstream<<int` variant: isolate integer-only insertion |  |
| `hello-ppu-oss-int-sigsegv` | `ostringstream<<int` variant: a SIGSEGV handler reads `oss+184` at fault time |  |
| `hello-ppu-oss-int-stack-vs-heap` | `ostringstream<<int` variant: stack vs heap allocation of the facet |  |
| `hello-ppu-oss-int-tracepoint` | `ostringstream<<int` variant: a runtime tracepoint to pin down the `oss+184` corruption timing |  |
| `hello-ppu-oss-layout` | memory-layout probe of `std::ostringstream` (a `sizeof` scan of the facet / buffer internals) |  |
| `hello-ppu-oss-manual-recipe` | replicate `_M_insert<long>` with an explicit vs implicit `ostreambuf_iterator` |  |
| `hello-ppu-oss-put-noargs` | `ostringstream` `put` variant: the facet reached through a const pointer (no-arg `put`) |  |
| `hello-ppu-oss-str-only` | `ostringstream` variant: isolate string-only insertion (`oss << "...") |  |
| `hello-ppu-ostream-iter-byval` | test `ostreambuf_iterator<char>` by-value through a virtual call (the sink + dummy check) |  |
| `hello-ppu-perf` | `<cell/perf/performance.h>` (`cellPerf*`) surface validation: `cellPerfGetVersion` / `Start` / `InsertCBEpmBookmark` / `Stop` |  |
| `hello-ppu-struct-byval` | test by-value passing of an 8-byte aggregate through a virtual call |  |
| `hello-ppu-struct-byval-ret` | test by-value return of an 8-byte aggregate through a virtual call |  |
| `hello-ppu-sv-aggregate-min` | C++ `std::vector` aggregate round-trip: a vector of 16-byte structs, push / read / clear |  |
### PSGL (`toolchain/hello-psgl-*`, `toolchain/hello-psglu`)

OpenGL ES 1.1-style rendering through `libPSGL` / `libPSGLU` on top of GCM.

The 15 samples with a 900-frame render loop (blend, cg-binary, cmdbuf,
ffp-lit-bootstrap, ffp-quad, ffp-shaderlib, glsl-object, msaa, multitexture,
rotating-quad, shader-interface, spudraw, spudraw-triangle, textured-quad,
triangle) share
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

`cgCreateProgram` creates a program from an embedded `.vpo`/`.fpo` buffer
(see `hello-psgl-cg-binary`). `psglLoadShaderLibrary` takes a path, so
samples that load an embedded shader library, or shaders through
`cgCreateProgramFromFile`, first write them to `/dev_hdd0/tmp`.

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
| `hello-psgl-cg-binary` | Offline-compiled `.vpo`/`.fpo` loaded with `cgCreateProgram` (from memory) and `cgCreateProgramFromFile` (from a file), with Cg error checks |
| `hello-psgl-glsl-object` | GLSL sources mapped to fixed Cg templates at build time |
| `hello-psgl-ffp-quad` | Fixed-function textured quad: texture upload, VBO with texcoords, orthographic projection |
| `hello-psgl-rotating-quad` | Fixed-function matrix stack animation |
| `hello-psgl-ffp-lit-bootstrap` | Fixed-function lighting: normals, one light, the built-in lit programs |
| `hello-psgl-ffp-shaderlib` | `--psgl-ffp-library` shader library + `psglLoadShaderLibrary`, textured quad |
| `hello-psgl-multitexture` | Fixed-function texturing on units 0-3: MODULATE, DECAL, ADD, BLEND and REPLACE, with a shader library for units 1-3 |
| `hello-psgl-vbo` | Buffer objects: create, size query, sub-data update, unmap, delete (no rendering) |
| `hello-psgl-cmdbuf` | Command-buffer record, call and return |
| `hello-psgl-spuhole` | Command-buffer hole sizing and alignment (no SPU) |
| `hello-psgl-spudraw` | SPU fills a command-buffer hole with NOPs; PPU replays it |
| `hello-psgl-spudraw-triangle` | SPU fills a hole with an indexed draw via `libspuPSGL` |
| `hello-psgl-fog-state` | Fog state set / query round trip (no rendering) |
| `hello-psgl-light-state` | Light, material and light-model state round trip (no rendering) |
| `hello-psglu` | `gluOrtho2Df`, `gluPerspectivef`, `gluLookAtf`, strings, `gluScaleImage` |

## gcm/

The diagnostic samples `hello-ppu-gcm-swizzle` and
`hello-ppu-gcm-fp-control` check CPU texture conversion and private command
emission respectively. Their READMEs distinguish those checks from GPU
rendering. For build-time DXT1/3/5 assets, see the native host example in
[`tools/dxt-encoder`](../tools/dxt-encoder/README.md).

[`hello-ppu-gcm-dxt`](gcm/hello-ppu-gcm-dxt/README.md) samples that example's
DXT1, DXT3 and DXT5 output on the RSX next to the uncompressed source, reads
frame 120 back and checks every source texel center (384 checks), and saves the
image and a receipt under `/dev_hdd0/tmp`. Its README gives the tolerances,
the PASS conditions and the bounded waits and exit drain.

RSX / cellGcm graphics.  Incremental: start at "clear the framebuffer",
grow toward shaders and full-pipeline samples.

### Host buffer

The host buffer passed to `cellGcmInit()` / `rsxInit()` holds the
command buffer and anything else the sample places in RSX-visible main
memory. Take it from LV2 with `gcm_host_buffer_alloc()` in
[`gcm/common/gcm_host_buffer.h`](gcm/common/gcm_host_buffer.h):

```
#include "../../common/gcm_host_buffer.h"

sys_addr_t host = 0;
if (gcm_host_buffer_alloc(HOST_SIZE, &host) != CELL_OK) return 1;
/* ... cellGcmInit(CB_SIZE, HOST_SIZE, (void *)(uintptr_t)host); ... */
```

Do not use `memalign(1 MiB, HOST_SIZE)`: the user sbrk arena is a fixed
64 MiB pool shared with the rest of the process, so a 16 or 32 MiB host
buffer from it costs the application that much heap. The 1 MiB LV2
pages are 1 MiB aligned, and the helper refuses sizes that are not a
multiple of 1 MiB. Once GCM has the buffer, keep it for the life of the
program; the RSX maps it and may still fetch from it. The pages still
count against the process's total memory; what the helper saves is
malloc arena space, not system memory.

Every sample and regression test that hands GCM a host buffer uses the
helper, including the FW framework, whose `destroy()` no longer frees
the mapped buffer. The upstream ports under `PSL1GHT/` keep their
original allocation.

[`hello-ppu-camera`](vision/hello-ppu-camera/README.md) captures 120 Eye
frames at VGA/30fps in packed YUV422, converts them on the PPU using integer
byte accesses, and presents a centered image through GCM. Its bounded run
reports capture and cleanup errors. RPCS3's Fake camera may supply no image
bytes; PS3/Eye capture validation requires a separate hardware observation.

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
| `hello-ppu-cellgcm-state-tour` | Self-checking tour of the native `cellGcm*` state calls: fifteen groups (scissor, depth, stencil, blend, colour mask, logic op, cull, fog, user clip plane, zcull, zpass report, vertex data, texture border, cache invalidation, AA and render enable) each render into a 64×64 tile and read probe pixels back against values computed from the state; prints `STATE_TOUR <group> PASS|FAIL expected=.. got=..` and `STATE_TOUR DONE pass=N fail=M`, exit 0 only when nothing fails | **builds** (hardware run pending) |
| `hello-ppu-prof-pixels` | Known answers for the RSX frame capture's passed samples per draw and zcull state: full- and half-screen quads, front-to-back and back-to-front depth pairs, a quad whose program kills half its samples, and a quad inside the app's own ZPASS occlusion query, with a zcull region bound over the depth buffer; requests a PIXELS, a plain and a BOTH \| PIXELS capture; `tools/check_pixels.py` checks them (host-tested) | **builds**, RPCS3 run matches (hardware run pending review) |
| `hello-ppu-prof-auto-verdicts` | Five 60-frame known-answer phases for automatic RSX profiler frame verdicts: RSX-bound (heavy fragment loops with `cellGcmSetWaitFlip`), PPU busy (~45 ms spin under HSYNC; the reader reports RSX not limiting, since a PPU-bound verdict needs PPU-busy evidence), pacing (VSYNC flip wait), RSX-bound flip-status polling, and RSX-bound label polling (`cellGcmSetWriteBackEndLabel` / `cellGcmIsLabelPassed`); prints `AUTOVERDICT phase <A|B|C|D|E> frame=<id>`, sealed with `PS3DK_PROF_AUTO_SEAL()` | **builds** (hardware run pending) |
| `hello-ppu-cellgcm-cube` | Rotating 3D cube.  8 vertices + 36-index buffer, per-PPU perspective × view × rotation matrix math, depth-test on so back-facing geometry occludes naturally | **green** (RPCS3 runtime test pending) |
| `hello-ppu-cellgcm-quad` | Textured quad.  4-vertex triangle strip, 64×64 procedural checker texture uploaded to RSX local memory, `cellGcmSetTexture` + sampler filter / address / control setup, fragment program with a `sampler2D` uniform bound to TEXUNIT0 | **green** (RPCS3 runtime test pending) |
| `hello-ppu-cellgcm-sysmenu` | Spinning triangle that pauses when the XMB system menu opens: the PPU freezes the rotation angle and overlays an animated "PAUSED" text strip (drifts + pulses via `cellDbgFont`); CLOSE resumes normal rendering, EXITGAME exits cleanly | **green** (RPCS3 runtime test pending) |
| `hello-ppu-dbgfont` | `cellDbgFont*` on-screen text overlay demo over a clear-loop background: multi-size / multi-colour text grid covering the whole screen, animated pulsing + drifting strings, per-frame counter | **green** (RPCS3 runtime test pending) |
| `hello-ppu-gcm-label-reuse` | A2 label reuse probe (light): verifies memory safety when reusing vertex/texture buffers after completion labels | **green** |
| `hello-ppu-gcm-label-reuse-heavy` | A2 label probe (heavy): index, vertex, and texture poison across early, command-label, and back-end completion modes under pipeline-saturating workload | **builds** (hardware measurement probe) |
| `hello-ppu-cellgcm-alpha-mask` | `rsx-cg-compiler` validation: FP alpha-mask if-else shape + implicit varying-binding inference through `cellGcmCg*` / `cellGcmSetFragmentProgram` |  |
| `hello-ppu-cellgcm-blend` | alpha-blend visual demo + `rsx-cg-compiler` emitter validation: three overlapping translucent quads |  |
| `hello-ppu-cellgcm-chain` | `rsx-cg-compiler` FP arithmetic-chain validation with per-draw runtime uniform patching |  |
| `hello-ppu-cellgcm-discard-blend` | `rsx-cg-compiler` validation: compound-discard + simple-discard + `VecConstruct` through the FP emitter |  |
| `hello-ppu-cellgcm-drawenv` | `setSurface` + `setDrawEnv` repro (mirrors the `cellgcm-clear` init / flip / clear loop) |  |
| `hello-ppu-cellgcm-global-uniforms` | rotating-cube textured sample: decodes `sdk/assets/ICON0.PNG` via `cellPngDec`, byte-shuffles to R8G8B8A8, samples it from a 3D cube |  |
| `hello-ppu-cellgcm-laneelision` | `rsx-cg-compiler` 3-unique-values lane-elision validation test |  |
| `hello-ppu-cellgcm-loops` | `rsx-cg-compiler` shader-feature validation (loops / control flow through the FP emitter) |  |
| `hello-ppu-cellgcm-render-to-texture` | render-target switch: after display init, allocate an offscreen surface, clear it, wait a fresh RSX completion label, read eight pixels back through the PPU mapping |  |
| `hello-ppu-cellgcm-sysinfo` | the GCM system-query surface, value-checked (the 28 `gcm_sys*` queries in `<rsx/gcm_sys.h>`) |  |
| `hello-ppu-cellgcm-textured-cube` | rotating cube, SDK ICON0 texture, canonical `flip_immediate`: decodes `ICON0.PNG` via `cellPngDec`, samples it from a 3D cube (24 vertices / 36 indices) |  |
| `hello-ppu-cellgcm-vp-loop` | basic GCM `flip_immediate` demo: `.cg` vertex + fragment shaders through `rsx-cg-compiler` |  |
| `hello-ppu-dbgrsx` | `cellDbgRsx` validation: `cellDbgRsxInit` + the debug resource-query surface |  |
| `hello-ppu-gcm-call-return` | GCM command segments: each frame's drawing is built into a small command segment and run via the CALL / RETURN segment APIs |  |
| `hello-ppu-gcm-main-arena` | textured, indexed quad from vertices / 16-bit indices / a texture in a 1 MiB `cellGcmMapMainMemory` arena; the PPU rewrites the arena between 60-frame phases and reads back 128x128 windows |  |
| `hello-ppu-gcm-nested-call` | nested-CALL probe: a ring segment CALLs an outer segment that itself CALLs an inner segment |  |
| `hello-ppu-gcm-zcull-clip-border` | GCM command coverage: 4 screen-space vertices (NDC + UV) drawn against a zcull region + clip border |  |
| `hello-ppu-resc` | `cellResc` surface validation: load `CELL_SYSMODULE_RESC`, exercise the render-surface-query API, print return codes |  |
| `hello-spu-gcm-segment` | SPU-built GCM command segments: each frame an SPU job writes the frame's drawing into a main-memory segment the PPU submits |  |

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
| `hello-ppu-input-combined` | Pad + keyboard + mouse polled in one loop; hot-plug on port 0 of each surface printed as it happens; any failed init, callback, read or shutdown call ends the run in FAIL (exit 1), while absent devices, idle frames and a confirmed unplug during a read are normal (`README.md`) | builds both ABIs; RPCS3 run of this revision pending; hot-plug needs a console |
| `hello-ppu-avc` | `cellSysutilAvc*` (legacy AVC voice-chat panel) surface validation: Load the panel, query show / mute / layout / volume, Unload |  |
| `hello-ppu-avc2` | `cellSysutilAvc2` (AVC2 voice-chat panel) init-param + surface-call validation via `sysutil_avc2_stub` |  |
| `hello-ppu-bgm` | `cellSysutil*BgmPlayback` (system-BGM) control surface: enable / disable / fade via the BGM-playback API |  |
| `hello-ppu-cellfs` | `cellFs` file-system surface validation: open / write / stat / read / memcmp / unlink round-trip of a small payload in `/dev_hdd0/tmp` |  |
| `hello-ppu-face` | `cellFaceDetection` surface validation: `cellFaceDetectionGetDictEa` + the face-detection call API |  |
| `hello-ppu-font` | `cellFont` surface validation: `cellFontInitializeWithRevision` / `GetRevisionFlags` + the font API |  |
| `hello-ppu-gamecontent` | `cellGame*` surface validation: all 17 entry points backed by `libcellGame_stub` |  |
| `hello-ppu-imejp` | `cellImeJpUtility` surface validation: all 41 entry points in `libsysutil_imejp_stub` |  |
| `hello-ppu-key2char` | `cellKey2Char` surface validation: all 5 entry points in `libcellKey2char_stub` |  |
| `hello-ppu-keyboard` | `cellKb` keyboard input surface: `<cell/keyboard.h>` end-to-end (`CellKeyboardInfo` / `Key` / data polling) |  |
| `hello-ppu-mouse` | `cellMouse` input surface: `<cell/mouse.h>` (`CellMouseInfo` / `ButtonState` polling) |  |
| `hello-ppu-msgdialog-3d` | the stereoscopic-3D "remove glasses" warning splash via `cellMsgDialogOpen2` (3D button type) |  |
| `hello-ppu-msgdialog-errcode` | `cellMsgDialogOpenErrorCode`: an error-code dialog that displays a formatted code + OK / Cancel |  |
| `hello-ppu-msgdialog-progressbar` | the progress-bar dialog via `cellMsgDialogOpen2` + `cellMsgDialogProgressBarInc` / `Set` |  |
| `hello-ppu-msgdialog-string` | a text-only dialog via `cellMsgDialogOpen2` with `TYPE_BUTTON_TYPE_NONE` |  |
| `hello-ppu-music-decode2` | `cellMusicDecodeUtility` v2 surface validation: all 10 v2 entry points |  |
| `hello-ppu-osk` | bring up the on-screen keyboard and exercise the late-SDK extension entry points (`SetLayoutMode` / `SetKeyLayoutOption`) |  |
| `hello-ppu-osk-ext` | `cellOskDialogExt*` extended-OSK validation: the 17-entry ext surface |  |
| `hello-ppu-pad` | `cellPad` API validation via the nidgen stub archive: `Pad*` (init / info / data / actuator) end-to-end |  |
| `hello-ppu-png` | decode a PNG via `cellPngDec` and display it via RSX (the decoder in `libcellPngDec_stub`) |  |
| `hello-ppu-rtc` | `cellRtc` validation: load the `cellRtc` SPRX, exercise the commonly used entry points (date / time / timezone) |  |
| `hello-ppu-savedata-link` | link-only test of the full `cellSaveData` Cell-name surface (takes the address of every export to confirm they resolve) |  |
| `hello-ppu-savedata-list` | `cellSaveDataListSave2` validation: present the savedata list dialog, offer a delete, confirm |  |
| `hello-ppu-search` | `cellSearch` content-search validation: init / search-start / finalize |  |
| `hello-ppu-storagedata` | drive `cellStorageDataImport` / `Export` / `ImportMove` with sentinel paths (no removable media required) |  |
| `hello-ppu-subdisplay` | `cellSubDisplay` surface validation: every entry point in `libsysutil_subdisplay_stub` |  |
| `hello-ppu-sync` | `libsync` + `libsync2` validation: load `cellSync` + `cellSync2`, exercise the sync / mutex / semaphore / condition surface |  |
| `hello-ppu-sys-event` | Cell-named LV2 event-queue / event-port surface: the `sys_*` snake_case wrappers |  |
| `hello-ppu-syscache` | mount the per-title cache area on `/dev_hdd1` (`cellSysCacheMount`), read back the assigned size, then clear |  |
| `hello-ppu-sysconf` | open the audio-device configuration sub-dialog (`cellSysconfOpen`) and wait for its callback |  |
| `hello-ppu-videoout-info` | the late-SDK `cellVideoOut*` surface (via `libsysutil_stub`): GetNumberOfDevice / Info / State / Configuration |  |
## sprx/

Relocatable modules (`ps3_add_prx`) and the programs that load them; see
[docs/tools/prx-gen.md](../docs/tools/prx-gen.md).

| Sample | Validates | Status |
|---|---|---|
| `module-export` | Module identity and entry points declared in C (`SYS_MODULE_INFO`, `SYS_MODULE_START`, `SYS_MODULE_STOP`), one exported library checked by `ps3_prx_export_roundtrip`, a loader that starts, calls, stops and unloads it | **green** + RPCS3 runtime-verified |
| [`module-segments`](sprx/module-segments/README.md) | The module layout retail Lv-2 needs (issue #137): code and data segments with the SPU permission bits, `0x1c` export records for two libraries plus the management record, and a `module_start`/`module_stop` that write and read back their own `.data` and `.bss`. The loader prints a verdict per step; ILP32 and LP64 | **green** + RPCS3 runtime-verified; console run pending |
| [`vsh-plugin`](sprx/vsh-plugin/README.md) | A VSH plugin for webMAN MOD's `/loadprx.ps3` / `/unloadprx.ps3`: `module_start` starts a worker with `sys_ppu_thread_create` and `module_stop` joins it, each ending with the bare exit system call and (stop) the final stop report when the thread has no caller. The worker logs to `/dev_hdd0/tmp` with `fsync` and counts in `.data` and `.bss`. The loader runs the game-process path twice; builds ILP32 and LP64, RPCS3 rows ILP32 | **green** + RPCS3 runtime-verified (loader path); console runs pending |

## lv2/

lv2 syscall-layer primitives: threading, synchronisation, event flags,
timers.  These samples use the platform ABI-name sync compat headers
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
| `hello-ppu-lv2-memcontainer` | LV2 memory container: four sequential 512 KiB allocate / nonzero-address / readback / free cycles from a 2 MiB container must all complete, total unchanged afterwards; invalid container IDs must be refused and `sys_memory_free(0)` returns EINVAL (`README.md`) | builds both ABIs; RPCS3 run of this revision pending |
| `hello-ppu-cellfs-async-stream` | cellFs AIO + streaming read on a scratch file it creates exclusively under a unique `/dev_hdd0/tmp` name and removes: `cellFsAioRead` with a callback (data checked), an AIO read plus `cellFsAioCancel` (outcome printed), then a `cellFsStRead` ring buffer over 32 checked 8 KiB blocks; an AIO request that does not complete in time stops the run with its resources left in place (`README.md`) | builds both ABIs; AIO and streaming need a console (RPCS3 stops on `cellFsAioInit`) |
| `hello-event-port-spu` | exercise the SPU side of lv2 event ports: `sys_spu_thread_send_event` / `throw_event` / `receive_event` / `tryreceive_event` / `group_yield` |  |
| `hello-ppu-crashdump` | `sys_crash_dump_*` validation: register a user log area, query it back, verify the crash-dump param shape |  |
| `hello-ppu-fiber` | `libfiber` bring-up validation: the PPU fiber surface end-to-end (fiber A / B scheduling) |  |
| `hello-ppu-lv2-mempool` | LV2 random-number, mempool and spinlock surface (`sys/random_number.h`, `sys/mempool.h`, `sys/spinlock.h`) |  |
| `hello-ppu-lv2-mmapper` | LV2 memory allocation and the mmapper wrapper (`sys_memory_alloc` / `sys_memory_free`) |  |
| `hello-ppu-lv2-timer` | an event loop driven by an LV2 kernel timer (`sys_timer_*`: a periodic timer posts one event per expiry) |  |
| `hello-ppu-lv2-vm` | on-demand memory: LV2 virtual memory (`sys_vm_*`) and the mmapper; reserve 32 MB, map, touch, unmap, free |  |
| `hello-ppu-lv2dbg` | `lv2dbg` syscall surface validation: load `CELL_SYSMODULE_LV2DBG`, exercise the low-level debug syscall surface |  |
| `hello-ppu-sheap` | `sheap` (shared-heap) surface validation: load `CELL_SYSMODULE_SHEAP`, exercise alloc / free |  |
| `hello-ppu-usbd` | `libusbd` bring-up validation: load `CELL_SYSMODULE_USBD`, exercise the `cellUsbd*` surface |  |
| `hello-sprx-export` | the build produces a SPRX from `exports.yml` (a `0x1c` function record) and the PPU loader reads the export table back to verify it |  |
| `hello-sprx-import` | import a function from a SPRX and call it (verifies ILP32 / LP64 container alignment + `ps3_add_prx` SIGN) |  |
## audio/

cellAudio mixer + cellAudioOut configuration / discovery.

[`hello-ppu-mic`](audio/hello-ppu-mic/README.md) captures bounded raw PCM
with per-channel integer peak/RMS meters and ordered cleanup in both ABIs.
Its default requires an Eye and exactly four channels at 48 kHz; generic USB
capture is explicit. Eye array capture remains pending hardware validation.

`hello-ppu-audio-multistream` uses three real stereo `cellAudio` ports for
independent tones, per-port gain phases and a shader-free gain display.
Its finite-run log checks submission and port progress; audible output is
validated separately. See its README for the `libmixer` placeholder limit.

| Sample | Validates | Status |
|---|---|---|
| `hello-ppu-audio` | cellAudio Init / PortOpen / PortStart / Stop / NotifyEventQueue / AddData chain; plays a chip-tune melody via direct ring fills against `libaudio_stub.a` | **green** + RPCS3 runtime-verified |
| `hello-ppu-audio-out` | cellAudioOut surface in `<cell/cell_audio_out.h>`: GetNumberOfDevice / GetDeviceInfo / GetState / GetConfiguration via `libsysutil_audio_out_stub.a`, then a 1 s 440 Hz sine tone through cellAudio so both the discovery API and the mixer downstream get exercised in the same boot | **green** + RPCS3 runtime-verified (nDevice=0 is an RPCS3 HLE quirk; FNIDs resolve and tone plays) |
| `hello-ppu-voice` | `cellVoice` surface validation: load `CELL_SYSMODULE_VOICE`, exercise the voice-control API, print return codes |  |
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
| `hello-daisy` | `libdaisy` data streams: seven local / atomic / signal-notification stream checks across PPU and SPU threads |  |
| `hello-ovis` | run two overlaid SPU programs (SPU image + overlay) and check their reports (size / fill / read-back) |  |
| `hello-ppu-ovis` | `cellOvis` overlay-table validation: query `cellOvisGetOverlayTableSize`, exercise the surface API |  |
| `hello-spu-dma` | the SPU writes a complete 16-byte DMA packet asynchronously to the PPU (mfc round-trip) |  |
| `hello-spu-simd` | SPU SIMD spin: vector math + a PPU-side check of the result |  |
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
| `hello-ppu-hl-spurs` | `hl/spurs.h` (PSL1GHT high-level `spurs::Spurs` C++ wrapper) bring-up + finalize, return-code logging |  |
| `hello-ppu-spurs-job-chain` | PPU-side job-chain bring-up: a 4-SPU Spurs2 instance + a tiny 2-entry job chain |  |
| `hello-spu-job` | SPURS JobChain + SPU job end-to-end: drive the JobChain machinery with an SPU job and inspect the raw job bytes |  |
| `hello-spu-spurs-umbrella` | PPU-side regression gate for the slim SPU `spurs.h` umbrella + the `lv2_event_queue.h` prototypes |  |
| `hello-spurs-getters` | validate libspurs SPU module-runtime getters: Spurs2 + Taskset, run an SPU task, query the getters |  |
| `hello-spurs-jq` | SPURS job-queue end-to-end: push N jobs through a `CellSpursJobQueueHandle` and collect their results |  |
| `hello-spurs-semaphore` | `CellSpursSemaphore` P/V validation: two SPU tasks share a 128-byte semaphore block |  |
| `hello-spurs-task2` | Taskset2 / Task2 API: Spurs2 bring-up (4 SPUs, `EXCLUSIVE_NON_CONTEXT`), class-1 taskset, three `createTask2` / `joinTask2` tasks returning 100 + number |  |
| `spurs-status` | run every spurs-suite row and show a live status board (one line per row: state, run time, verdict + a live panel for the running row) |  |
| `sync2-objects` | SPURS tasks, a SPURS job and PPU threads sharing `cellSync2` objects |  |

## PSL1GHT/

| Sample | Validates | Status |
|---|---|---|
| `audiosync` | PSL1GHT port: `cellAudio` playback (`audioInit` / ring-buffer fill with a 48 kHz tone, `cellAudioAddData`) | audiotest |
| `audiotest` |  |  |
| `blitting` |  |  |
| `gemsample` |  |  |
| `gemtest` |  |  |
| `kbtest` |  |  |
| `padtest` |  |  |
| `debugtest` |  |  |
| `echoserv` |  |  |
| `httptest` |  |  |
| `networktest` |  |  |
| `spuchain` |  |  |
| `spudma` |  |  |
| `spumars` |  |  |
| `spuparallel` |  |  |
| `spurs` |  |  |
| `sputest` |  |  |
| `sputhread` |  |  |
| `msgdialog` |  |  |
| `osk` |  |  |
| `threadtest` |  |  |


## codec/

| Sample | Validates | Status |
|---|---|---|
| `hello-ppu-adec` | `cellAdec` (audio-decoder) framework validation: Init / QueryAttr / Open / Close round-trip requesting an LPCM decoder |  |
| `hello-ppu-atrac3multi` | `cellAtracMulti` (`<cell/codec/libatrac3multi.h>`) decoder validation: load + the ATRAC3-mux surface |  |
| `hello-ppu-atrac3plus` | `cellAtrac` (ATRAC3+) validation: minimal init / query / create / delete round-trip |  |
| `hello-ppu-celp8enc` | `cellCelp8Enc` (`<cell/codec/celpenc8.h>`) validation: QueryAttr to Open to Close |  |
| `hello-ppu-celpenc` | `cellCelpEnc` (`<cell/codec/celpenc.h>`) validation: QueryAttr to Open to Close |  |
| `hello-ppu-dmux` | `cellDmux` + PAMF demux chain: the PAMF message-callback dispatches `DEMUX_DONE` / `FATAL_ERR` / `PROG_END_CODE` through the SPRX |  |
| `hello-ppu-gifdec` | `cellGifDec` (`<cell/codec/gifdec.h>`) validation: Create / Open / Close |  |
| `hello-ppu-jpg-dec` | decode a JPEG via `cellJpgDec` and display it via RSX; the full libjpgdec lifecycle |  |
| `hello-ppu-jpgenc` | buffer-only `cellJpgEnc` regression sample: generate a small RGB test image in memory and encode it |  |
| `hello-ppu-pamf` | `cellPamf` container-parser validation: feed the SPRX a minimal PAMF-magic buffer; check the unsupported-version refusal |  |
| `hello-ppu-pngenc` | `cellPngEnc` validation: QueryAttr to Open to Close |  |
| `hello-ppu-sail` | `cellSail` validation: MemAllocator init (avoids the Player state machine) |  |
| `hello-ppu-sail-rec` | `cellSailRec` validation: Composer init / finalize (the 4.75+ surface) |  |
| `hello-ppu-vpost` | `cellVpost` (post-processing) validation: QueryAttr to Open to Close round-trip |  |


## dbgfont/

| Sample | Validates | Status |
|---|---|---|
| `hello-ppu-dbgfont-console` | exercise the Console API subsystem: open a custom console (lower-right, green) alongside the auto stdout console and write to both every frame via `cellDbgFontDrawGcm` |  |


## debug/

| Sample | Validates | Status |
|---|---|---|
| `hello-debug-rpcs3` | a small program to practise GDB on in RPCS3 (the target stops at `accumulate()` for `bt` / `info locals` / `print *p`) |  |


## font/

| Sample | Validates | Status |
|---|---|---|
| `hello-ppu-font-render` | draw text with the system font and show it: `cellFont` (canonical) on line 1 and the PSL1GHT `font*` wrappers on line 2 |  |
| `hello-ppu-fontgcm` | `cellFont` GCM path: `cellFontInitGraphicsGcm` renders a text line through the GCM command buffer |  |


## fw/

| Sample | Validates | Status |
|---|---|---|
| `hello-ppu-fw-gcm-text` | GCM-backed FW text rendering: `FWGCMApplication` owns a real `FWCellGCMWindow` over cellGcm; `FWGCMDebugFontRenderer` adapts to `libdbgfont`; emits visible text every frame for 60 frames |  |
| `hello-ppu-fw-headers` | header-only FW compatibility surface validation: macro / config / typedef shape (no runtime calls) |  |
| `hello-ppu-fw-input` | FW input validation: the Tier 4 sample input facade over the FW app lifecycle |  |
| `hello-ppu-fw-null-lifecycle` | Null FW lifecycle + debug-font facade: the no-op shim path through the FW app / renderer interfaces |  |


## network/

| Sample | Validates | Status |
|---|---|---|
| `hello-ppu-http` | `cellHttp` module-load validation: load `CELL_SYSMODULE_NET` + `CELL_SYSMODULE_HTTP`, print load / release return codes |  |
| `hello-ppu-libnet` | `cellLibnet` module-load validation: load `CELL_SYSMODULE_NET` via the nidgen stub archive, print return codes |  |
| `hello-ppu-netctl` | `cellNetCtl` module-load validation: load `CELL_SYSMODULE_NETCTL`, print the load return code |  |
| `hello-ppu-np` | `cellNp` module-load validation: load `CELL_SYSMODULE_NP`, print the load return code |  |
| `hello-ppu-np-clans` | `cellNpClans` module-load validation: load `CELL_SYSMODULE_CLANS`, print the load return code |  |
| `hello-ppu-np-commerce2` | `cellNpCommerce2` module-load validation: load `CELL_SYSMODULE_COMMERCE2`, print the load return code |  |
| `hello-ppu-np-sns` | `cellNpSns` module-load validation: load `CELL_SYSMODULE_SNS`, print the load return code |  |
| `hello-ppu-np-trophy` | `cellNpTrophy` module-load validation: load `CELL_SYSMODULE_TROPHY`, print the load return code |  |
| `hello-ppu-np-tus` | `cellNpTus` module-load validation: load `CELL_SYSMODULE_TUS`, print the load return code |  |
| `hello-ppu-np-util` | `cellNpUtil` module-load validation: load `CELL_SYSMODULE_NP` + the `cellNpUtil` surface, print return codes |  |
| `hello-ppu-np2` | `cellNp2` module-load validation: load `CELL_SYSMODULE_NP2` (`<cell/np/np2.h>`), print the load return code |  |
| `hello-ppu-rudp` | `cellRudp` raw-datagram surface validation: load `CELL_SYSMODULE_RUDP`, exercise the socket-API entry points |  |
| `hello-ppu-ssl` | `ssl` module-load validation: load `CELL_SYSMODULE_SSL`, exercise the certificate / handshake surface |  |

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

It is the ABI measurement: if it regresses, our output drifted from the
system loader's expectations.

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

Final artefacts land under the build directory (`cmake-build/` in the example
above), keeping sample source trees completely clean across different ABIs.
To restore the legacy layout where outputs land next to `CMakeLists.txt`, set
`set(PS3_SELF_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")` in your `CMakeLists.txt`
or pass `OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"` to `ps3_add_self` / `ps3_add_prx`.
An explicit `OUTPUT` path to `ps3_add_prx` takes precedence over those directory
settings. The PNG sample keeps its boot asset beside the selected SELF output.

`scripts/sweep-samples.ps1` retains each complete build in its named run directory.
It refuses older installed helpers that still use source-directory defaults.
Pass that exact run directory to `scripts/boot-sweep.ps1 -sweepDir`; artifact lookup
does not fall back to source files or another run.

Each sample produces three artefacts:

| Artefact | Target | How to launch |
|---|---|---|
| `<sample>.elf` | Stripped PPU ELF | `ps3load` / GDB against RPCS3's gdbstub / `rpcs3 cmake-build/<sample>.elf` |
| `<sample>.self` | CEX-signed SELF | `rpcs3 --no-gui cmake-build/<sample>.self` |
| `<sample>.fake.self` | Fake-signed SELF | `ps3load` over LAN / XMB install on CFW hardware |

**RPCS3 (preferred for quick iteration):**
```bash
rpcs3 --no-gui cmake-build/<sample>.self
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
2. `toolchain/hello-ppu-abi-check` — ABI measurement.
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
