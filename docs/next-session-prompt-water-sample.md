# Next session — Water tutorial sample (gaps survey)

We staged the GCM/CgTutorial Water sample (the larger reflection +
refraction tutorial — six shaders, libfwgcm + gtfloader + gcmutil
deps) under `~/sony-build/cell-sdk-shim/samples/tutorial/CgTutorial/GCM/Water/`
and walked it through build → fself → RPCS3.  This file lists what
we had to patch to get there and what's still failing, so the next
session can pick up from a known state.

Build artifact: `Water.ppu.elf` (7.3 MB) and `Water.ppu.self`.  `make`
goes green.  RPCS3 boots the SELF, finishes module registration, maps
RSX local memory, then the sample calls `Abort called.` from a
failed `_sys_lwmutex_lock` (CELL_ESRCH).

## Gaps closed in this session (already on disk)

1. **`samples/fw/Makefile` — drop `fw.mk`.**  Built `MK_TARGET = fwgcm.mk`
   only; `fw.mk` (the PSGL `libfw.a` variant) references `glFinish` /
   `pWindow` that don't exist in the GCM-only SDK.  Reference samples
   only ever link `libfwgcm.a`, so this is the right scope.
2. **`sce-cgc` shim — fall back to `wine sce-cgc.exe` on rsx-cg-compiler
   miss.**  `rsx-cg-compiler` exits 0 on emit failure (just prints
   "NV40 emit: not ready (skeleton stage)") and writes nothing.
   The shim now checks for non-empty output, and if missing, retries
   under `wine` against `~/cell-sdk/.../host-win32/Cg/bin/sce-cgc.exe`.
   Lets reference samples build while we close gaps in our compiler.
3. **`reference/sony-sdk/samples/mk/sdk.makedef-ppu-gcc.mk` — auto-link
   `-lsysutil` and `-lsimdmath`.**  `vectormath/cpp/{vec,mat}_aos.h`
   reference `rsqrtf4` / `recipf4` / `negatef4` / `sincosf4` (live
   in `libsimdmath.a`); `cell/cell_video_out.h` inline forwarders
   call PSL1GHT names `videoGetState` / `videoConfigure` (live in
   `libsysutil.a`, not `libsysutil_stub.a`).  The reference SDK's
   spec auto-linked these; ours didn't.  Now `GCC_PPU_LOADLIBS =
   -lsysutil -lsimdmath`.
4. **Stub-archive name aliases** (manual symlinks under
   `stage/ps3dev/ps3dk/ppu/lib/`).  Reference Makefiles spell them
   `_stub`, our SDK install drops the suffix:
   - `libgcm_sys_stub.a → libgcm_sys.a`
   - `libio_stub.a → libio.a`
   - `libusbd_stub.a → libusb.a`
5. **`make_fself` host tool symlink.**  Reference Makefile invokes
   `$(CELL_HOST_PATH)/bin/make_fself`; our toolchain ships `fself`.
   Symlinked under shim's `host-linux/bin/make_fself → fself`.
6. **`sprxlinker` post-link step.**  Reference `sdk.target-ppu.mk`
   goes ELF → SELF directly (no SPRX-binding patch step).  PSL1GHT's
   `ppu_rules` runs `sprxlinker $elf` before the SELF.  Without it,
   stub trampolines bctrl into `0x7c0802a6` (`mflr r0` byte sequence
   — uninitialized resolution slot).  We ran `sprxlinker` manually
   on `Water.ppu.elf` and the boot hits `_sys_prx_register_module`
   correctly; reference build flow needs an automatic sprxlinker
   stage in `sdk.target-ppu.mk`.

## Open gaps (next-session work)

### A. SDK install — make name aliases permanent

`scripts/build-cell-stub-archives.sh` already ships
`libgcm_sys.a` (renamed from `libgcm_sys_stub.a`) but doesn't create
the `_stub`-suffixed alias.  Same for `libio.a` and `libusb.a` (the
latter from `libpsl1ght-side`, not built by `build-cell-stub-archives.sh`).
Add the symlinks (or duplicate-named copies) at install time so
reference Makefiles stop tripping.

### B. SDK headers — drop PSL1GHT forwarder layer in cell/cell_video_out.h

`sdk/include/cell/cell_video_out.h` defines five inline forwarders
that call PSL1GHT shim names (`videoGetState`, `videoConfigure`,
`videoGetResolution`, `videoGetConfiguration`,
`videoGetResolutionAvailability`).  These resolve via
`libsysutil.a` (PSL1GHT user shim), not `libsysutil_stub.a` (Cell
SPRX stubs that the Water Makefile actually pulls in).  The
inline forwarders should call the `libsysutil_stub.a`-provided
`cellVideoOut*` externs directly, so reference Makefiles that link
only `-lsysutil_stub` resolve correctly without the workaround
auto-link of `-lsysutil` we added in step 3 above.

### C. rsx-cg-compiler — Water shaders need three new code paths

All six Water shaders fall through to `wine sce-cgc.exe`.  The
distinct gaps:

1. **VP — top-level `float4x4` uniform binding.**  Both
   `VertexProgram.cg` and `SimpleVertex.cg` declare
   `float4x4 gVertexModelViewProjectionMtx;` and dot-product against
   `position` via `mul()`.  Compiler errors with
   `nv40-vp: LoadUniform for unknown global 'gVertexModelViewProjectionMtx'`.
   The sample just outputs `mul(M, position)` and `worldPos = position`
   — should be a baseline VP we already cover.  Likely a
   uniform-table-lookup bug for top-level globals (vs file-scope
   `: register(CN)`-tagged ones, which we already pass).
2. **FP — uniform+uniform / uniform+literal arithmetic.**
   `FragmentProgram.cg` builds local consts like
   `const float3 frequency = 2.0 * 3.14159 / waveLength;` — pure
   uniform/literal arithmetic that gets folded into a constant.
   Compiler errors with
   `nv40-fp: arithmetic ops supported only for (varying, uniform) pairs (uniform+uniform / literal arithmetic lands later)`.
   The "lands later" is the explicit deferred path we still owe.
3. **FP — unsupported IR op #84.**  Hits in `RenderSkyBoxFragment.cg`
   (`texCUBE` sample) and `SimpleFragment.cg` (mix of `tex2D` +
   conditional output).  Need to look up which IR op `#84` is in
   the compiler — might be `texCUBE`-style sampler call, might be
   a `lerp` / `mix`-style intrinsic.

Also: **rsx-cg-compiler exits 0 on emit failure.**  After printing
`NV40 emit: not ready (skeleton stage).`, the binary returns
success.  Build systems can't detect the miss without checking
output-file size.  Should exit non-zero.

### D. Runtime — `_sys_lwmutex_lock` CELL_ESRCH after RSX init

Boot trace (from `~/.cache/rpcs3/RPCS3.log`):

```
sys_memory_allocate(...)                       # OK
_sys_prx_register_module("cellProcessElf")     # OK
_sys_prx_register_module("_sysProcessElf")     # OK
sys_mmapper_allocate_address(size=0x10000000)  # RSX VM, OK
_sys_lwmutex_lock failed with 0x80010005 : CELL_ESRCH
sys_tty_write: "Abort called."
```

`CELL_ESRCH` on lwmutex_lock means the lwmutex ID is invalid /
uninitialized.  Could be:

- An lwmutex field in a global / static C++ object whose constructor
  ran before the lwmutex creation syscall was wired up.
- A libfwgcm or gcmutil global lock that needs initialization in a
  startup path we haven't ported.
- An `ATTRIBUTE_PRXPTR` mismatch shifting a struct field so the
  lwmutex ID slot is reading garbage from elsewhere.

Disassemble around `0x57f14` (caller PC at the failed lwmutex_lock)
to find which fwgcm / gcmutil object holds the mutex.  Compare the
struct layout against the reference SPRX expectation — the prior
"struct pointer fields must be `ATTRIBUTE_PRXPTR`" lesson likely
applies here too.

### E. (Lower priority) `samples/fw/fw.mk` PSGL variant

`samples/fw/src/cell/FWCellMain.cpp` references `pWindow`,
`glFinish` from the PSGL backend; we don't ship PSGL.  Either drop
`fw.mk` from the shim permanently (current state — works), or
build a no-op PSGL skeleton if any future sample needs `libfw.a`.
None of the GCM tutorial samples link `libfw.a`.

## Repro

```bash
source ~/source/repos/PS3_Custom_Toolchain/scripts/env.sh
cd ~/sony-build/cell-sdk-shim/samples/tutorial/CgTutorial/GCM/Water
CELL_SDK=$HOME/sony-build/cell-sdk-shim make             # builds Water.ppu.elf + .self
$PS3DEV/bin/sprxlinker Water.ppu.elf                     # required (manual today)
$HOME/sony-build/cell-sdk-shim/host-linux/bin/make_fself \
    Water.ppu.elf Water.ppu.self                         # repacks SELF
rpcs3 --no-gui Water.ppu.self                            # boots, then Abort.
```

`~/.cache/rpcs3/RPCS3.log` is the post-mortem.
