# Plan: Modern PS3 Custom Toolchain (C++17, PPU + SPU)

## Context

Build a modern, feature-complete open-source PS3 SDK that supports C++17 on both the PowerPC 64 PPU (PPE) and the Synergistic Processing Units (SPU) of the IBM Cell Broadband Engine. The ps3dev baseline (ps3toolchain + PSL1GHT + ps3libraries) is stale — master is still on GCC 7.2.0 / binutils 2.22 / newlib 1.20.0 — and PSL1GHT has known gaps (fragment shaders, networking, ad-hoc naming that doesn't match the reference ABI). User is a former PS3 platform licensee with legal copies of a proprietary reference SDK, which will be used privately as a coverage oracle (read-only, never shipped).

The core technical risk is SPU backend support: GCC 9 marked it obsolete, GCC 10.1 removed `gcc/config/spu/` entirely (~34,000 lines). However, **binutils 2.42 still ships `spu-elf`**, **newlib still ships `libgloss/spu`**, and **GCC 9.5.0 has both the C++17 frontend and an intact SPU backend**. This makes a hybrid ship path feasible: modern PPU on GCC 12.4.0, SPU on GCC 9.5.0 in parallel, with a long-lead forward-port of the SPU backend to GCC 12/13 running independently.

Intended outcome: a reproducible MSYS2-native toolchain under `$PS3DEV` that produces valid PS3 SELF/SPRX binaries from modern C++17 sources, a PSL1GHT v3 with Cell-style naming and auto-generated stub libraries (NID/FNID driven), fragment-shader-capable RSX, and a ≥95% coverage matrix against the reference SDK for the subsystems homebrew actually needs.

---

## Locked-in Decisions (from Q&A)

| Decision | Choice |
|---|---|
| Build host | **Native MSYS2 MinGW64** on Windows 11 |
| SPU strategy | **Option C Hybrid** — Phase 2a ships GCC 9.5.0 SPU; Phase 2b forward-ports `config/spu/` to GCC 12+ in parallel |
| PPU GCC | **GCC 12.4.0** |
| Fragment shader scope | **NV40-FP assembler first** (full GLSL/Cg compiler is a later deliverable) |
| reference SDK mount | **Deferred** — user provides path later; `reference/private/` is a placeholder until then. Phase 3 coverage tooling + stub verification blocks on this. |
| NID/stub tooling language | **Rust** (single static binary, strong typing, one-shot CI artifact) |
| PSL1GHT v3 RFC naming | **Ship with `psl1ght-compat.h` shim** for 1–2 releases so existing homebrew can migrate |

---

## Workspace Layout

Everything lives under `C:\Users\FirebirdTA01\source\repos\PS3_Custom_Toolchain\`:

```
PS3_Custom_Toolchain\
├── README.md, LICENSE, NOTICE, .gitignore, .gitattributes
├── scripts\          bootstrap.sh, env.sh, build-*.sh, clean.sh
├── src\              .gitignored. upstream\ (gcc,binutils,newlib mirrors), ps3dev\, forks\, tarballs\
├── patches\          ppu\{gcc-12.x, binutils-2.42, newlib-4.x, gdb-14.x}\*.patch
│                     spu\{gcc-9.5.0, binutils-2.42, newlib-4.x}\*.patch
│                     psl1ght\ portlibs\
├── tools\            nidgen\ (Rust), stubgen\ (Rust), coverage-report\ (Rust)
├── reference/                   private/ (read-only symlink; .gitignored; deferred mount)
├── docs\             quickstart, migration, abi-reference, spu-programming, rsx-programming, coverage
├── build\            .gitignored. ppu\, spu\, psl1ght\, portlibs\
├── stage\ps3dev\     the $PS3DEV prefix. bin\, ppu\, spu\, psl1ght\, portlibs\
├── samples\          hello-ppu-c++17, hello-spu, ppu-spu-dma, rsx-triangle-vs, rsx-textured-quad-fs, net-http-get
├── ci\               docker\linux.Dockerfile, docker\msys2.Dockerfile, github-actions\
└── tests\            smoke\, conformance\, hardware\, rpcs3\
```

MSYS2-specific requirements baked into `scripts/env.sh`: enable Windows long paths (`HKLM\SYSTEM\CurrentControlSet\Control\FileSystem\LongPathsEnabled=1`), force `core.autocrlf=false`, mark `patches/` as `-text` in `.gitattributes`, use short build root (build trees alias to `C:\ps3tc\build\` to avoid MAX_PATH).

---

## Phase 0 — Bootstrap

**Effort:** Low. **Duration:** 2–4 days.

Create the workspace scaffolding, clone all upstream + ps3dev + fork repos, pin release tags, document MSYS2 dependency install.

### Clone list (exact URLs + pinned refs)

Upstream (into `src/upstream/`, shallow `--filter=blob:none`):
- `git://sourceware.org/git/gcc.git` — checkout `releases/gcc-12.4.0` and `releases/gcc-9.5.0`
- `git://sourceware.org/git/binutils-gdb.git` — `binutils-2_42`, `gdb-14.2-release`
- `git://sourceware.org/git/newlib-cygwin.git` — `newlib-4.4.0.20231231`

ps3dev (into `src/ps3dev/`):
- `https://github.com/ps3dev/ps3toolchain` — master
- `https://github.com/ps3dev/PSL1GHT` — master (also fetch PR/branch for Issue #67 v3 RFC)
- `https://github.com/ps3dev/ps3libraries` — master

Forks for patch provenance (into `src/forks/`):
- `https://github.com/bucanero/ps3toolchain` — active maintenance
- `https://github.com/jevinskie/ps3-gcc` — latest-and-greatest GCC experiments
- `https://github.com/Estwald/PSDK3v2` — Windows packaging reference
- `https://github.com/StrawFox64/PS3Toolchain` — alternate patch lineage
- PSX-Place 2023 GCC 9.5.0 fork (locate canonical URL during bootstrap)

### MSYS2 MinGW64 deps

In `scripts/bootstrap.sh`: `pacman -S --needed base-devel mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-gmp mingw-w64-x86_64-mpfr mingw-w64-x86_64-mpc mingw-w64-x86_64-isl mingw-w64-x86_64-python mingw-w64-x86_64-rust git wget bison flex texinfo`.

### reference SDK mount (deferred)

When user provides path: junction it with `cmd /c mklink /J reference/private "<user-path>"`, lock it read-only via `icacls reference/private /deny %USERNAME%:(WD,DC)`, add pre-commit hook that rejects any diff touching `reference/private/`.

### Critical files

- `scripts\bootstrap.sh` (new)
- `scripts\env.sh` (new) — exports `PS3DEV`, `PPU_PREFIX`, `SPU_PREFIX`, PATH
- `.gitignore` (new) — excludes `src/`, `build/`, `stage/`, `reference/private/`
- `.gitattributes` (new) — `patches/** -text`, CRLF rules

---

## Phase 1 — PPU Toolchain (GCC 12.4.0, C++17)

**Effort:** High. **Duration:** 4–8 weeks.

Build `powerpc64-ps3-elf-{gcc,g++,ld,gdb}` producing C++17 binaries that run on RPCS3 and real hardware.

### Target versions

| Component | Version |
|---|---|
| GCC | 12.4.0 |
| binutils | 2.42 |
| newlib | 4.4.0.20231231 |
| GDB | 14.2 |
| GMP / MPFR / MPC / ISL | 6.3.0 / 4.2.1 / 1.3.1 / 0.26 |

### Patch rebase: `gcc-7.2.0-PS3.patch` → GCC 12.4.0

The existing ps3dev patch (see `src/ps3dev/ps3toolchain/patches/gcc-7.2.0-PS3.patch`) does three things: adds `powerpc64-ps3-elf` to `gcc/config.gcc`, installs `gcc/config/rs6000/cell64lv2.h`, tweaks `libstdc++-v3` and `libgcc` build glue. Expected conflicts rebasing to 12.4.0:

- `gcc/config.gcc` — `powerpc64*-*-*` block was rewritten between GCC 7→10 (TLS default, `enable_secureplt`, libsanitizer toggles). Re-apply target stanza by hand.
- `gcc/config/rs6000/rs6000.c` → split into `rs6000-call.cc`, `rs6000-logue.cc` etc. in GCC 8 and renamed `.c`→`.cc` in GCC 12. Any hunks targeting the monolithic file must be relocated.
- `libstdc++-v3/crossconfig.m4` — rewritten in GCC 10 for C++20. Add `powerpc64-ps3-elf` arm: newlib-style target, disable libbacktrace initially, disable filesystem TS.
- Regenerate with `autoreconf -i` in `libstdc++-v3/`; do not hand-patch generated `configure`.

### newlib 4.4.0 PS3 port

Existing `newlib-1.20.0-PS3.patch` adds `libgloss/libsysbase` PS3 glue (open/close/read/write/lseek/fstat/sbrk/_exit → lv2 syscall wrappers) and `newlib/libc/sys/ps3/` CRT0 + heap setup. Port to newlib 4.x as patch series:

1. `patches/ppu/newlib-4.x/0001-add-ps3-host.patch` — `configure.host`, `libgloss/configure.ac`
2. `0002-libsysbase-ps3-stubs.patch` — syscall table, `libgloss/ps3/*.S`
3. `0003-crt0-heap-setup.patch` — `__heap_start` / `__heap_end` linker symbols
4. `0004-reent-stubs.patch` — minimal reent wrapping lv2 TLS
5. `0005-pthread-stubs.patch` — weak symbols for gthr wiring

### Threading strategy for `std::thread`

Ship in two steps:

- **Phase 1d (first shipping milestone):** `--enable-threads=single`. `std::thread` throws `system_error`. Unblocks everything else.
- **Phase 1e:** Add libstdc++ gthr shim: `__gthread_create` → `sys_ppu_thread_create`, `__gthread_join` → `sys_ppu_thread_join`, mutex/cond/once wrappers. ~300 LOC of C in newlib's libsysbase. Rebuild libstdc++ against threads=posix. Pin a TOOLCHAIN_ABI_VERSION constant.

### Validation

1. Stage-1 GCC (C only) builds → newlib builds → stage-2 GCC (C/C++) builds against newlib.
2. `samples/hello-ppu-c++17/` uses `std::vector`, `std::string_view`, structured bindings, `if constexpr` — builds with `-std=c++17`, links libstdc++, runs on RPCS3.
3. `-std=c++17` default accepted; `std::thread` creates and joins (Phase 1e).
4. GDB 14.2 connects to RPCS3's gdbstub and single-steps PPU code.

### $PS3DEV/ppu layout

`stage/ps3dev/bin/{powerpc64-ps3-elf-gcc,g++,ld,as,ar,objcopy,gdb}` + `ppu-*` aliases; `stage/ps3dev/ppu/powerpc64-ps3-elf/{include,lib,sys-include}`; `stage/ps3dev/ppu/lib/gcc/powerpc64-ps3-elf/12.4.0/{libgcc.a,libstdc++.a,libsupc++.a}`.

### Risks

- **ELFv1 vs ELFv2 / long double width.** PS3 uses ELFv1. Force `-mabi=elfv1 -mlong-double-64` in `cell64lv2.h`; verify after every GCC bump.
- **AltiVec/VMX codegen regressions.** Default `-mno-altivec`; opt-in per project.
- **PIC for PRX loading.** Spec file must add `-fPIC` to libgcc/libstdc++ build flags.
- **Patch rebase explodes beyond ~2000 LOC.** Fallback: target GCC 11.5.0 (smaller delta).

### Critical files

- `patches\ppu\gcc-12.x\0001-add-powerpc64-ps3-elf-target.patch` (rebased target triple)
- `patches\ppu\gcc-12.x\0002-cell64lv2-header.patch` (new file `gcc/config/rs6000/cell64lv2.h`)
- `patches\ppu\gcc-12.x\0003-ps3-libstdcxx-tweaks.patch` (crossconfig.m4, threads gthr)
- `patches\ppu\newlib-4.x\0002-libsysbase-ps3-stubs.patch` (load-bearing)
- `scripts\build-ppu-toolchain.sh` (orchestrator)

---

## Phase 2 — SPU Toolchain (Option C Hybrid)

### Phase 2a: Ship with GCC 9.5.0 for SPU

**Effort:** Medium. **Duration:** 3–5 weeks. Can run in parallel with Phase 1 after Phase 0.

| Component | Version |
|---|---|
| GCC (SPU) | 9.5.0 |
| binutils | 2.42 (shared with PPU) |
| newlib | 4.4.0 (shared with PPU) |

Build flow (`scripts/build-spu-toolchain.sh`):

1. binutils 2.42 `--target=spu-elf --program-prefix=spu- --prefix=$PS3DEV`. No GCC-side risk — binutils still supports spu-elf intact.
2. GCC 9.5.0 stage-1 C: `--target=spu-elf --with-newlib --disable-threads --disable-libssp --disable-shared`.
3. newlib 4.4.0 for spu-elf. Apply `patches/spu/newlib-4.x/0001-libgloss-spu-ps3.patch` — thin, adds PS3-specific syscall glue for `spu_thread_receive_event` etc.
4. GCC 9.5.0 stage-2 C/C++: `--enable-languages=c,c++ --disable-libstdcxx-pch --disable-libstdcxx-filesystem-ts`. Default spec file forces `-fno-exceptions -fno-rtti` for SPU (256 KB LS budget).
5. Install as `spu-elf-*` and `spu-*` aliases.

Ship an explicit linker script at `stage/ps3dev/spu/share/spu_ps3.ld` that defines the 256 KB LS layout (`.text` at 0x0, `.data`, `.bss`, stack from top descending, top 16 KB reserved for DMA/mailbox queues).

### Phase 2a validation

- `samples/hello-spu/` — mailbox printf from SPU to PPU.
- `spu_add`, `spu_sub`, `spu_splats` intrinsics emit correct si-insns.
- DMA via `mfc_get` / `mfc_put` wrappers compiles and links.
- SPU ELF fits in 256 KB LS with `-Os -fno-exceptions -fno-rtti`.
- Load SPU ELF from PPU via `sys_spu_image_import` path; runs in RPCS3 SPU interpreter.
- C++17 smoke: `constexpr`, `std::array`, `std::tuple` (header-only bits).

### Phase 2a risks

- newlib libc too large for LS → provide `libspu_mini.a` (reimplement printf/memcpy/memset/strcmp in ~4 KB).
- Vendor `spu_intrinsics.h` has extensions beyond IBM CBEA → catalog vendor-only intrinsics, provide emulation in `PSL1GHT/spu/include/spu_intrinsics_ps3_extras.h`.

### Phase 2b: Forward-port `config/spu/` to GCC 12.4.0 (long-lead, parallel)

**Effort:** Very High. **Duration:** 12–24 weeks. Runs in its own worktree; does **not** block shipping M0–M8.

Lift `gcc/config/spu/` from GCC 9.5.0 verbatim into GCC 12.4.0 tree. Main adaptation surfaces:

- `machmode.def` mode class refactor (GCC 11+) — remap `V16QImode`, `V8HImode`, `V4SImode`, `V4SFmode`, `V2DImode`, `V2DFmode`.
- Target hook signature changes across ~30–50 hooks (`TARGET_VECTOR_MODE_SUPPORTED_P`, `TARGET_LEGITIMATE_ADDRESS_P`, etc.).
- Builtin infrastructure moved to gimple-folding in GCC 12 — update `spu-builtins.def` + matching expanders.
- DWARF CFI verification for SPU's unusual calling convention (`INCOMING_FRAME_SP_OFFSET`, `DWARF_FRAME_REGNUM`).
- Disable autovec on SPU (modern vectorizer assumes SSE/AVX/NEON predicate models that SPU lacks).

Accept up front that this is a permanent downstream fork — upstream reacceptance is extremely unlikely. GCC 9.5.0 stays as the eternal fallback.

### Critical files (Phase 2)

- `scripts\build-spu-toolchain.sh` (orchestrator)
- `patches\spu\gcc-9.5.0\0001-spu-ps3-libgcc-tweaks.patch`
- `patches\spu\newlib-4.x\0001-libgloss-spu-ps3.patch`
- `stage\ps3dev\spu\share\spu_ps3.ld` (linker script)

---

## Phase 3 — PSL1GHT Modernization

**Effort:** High. **Duration:** 6–10 weeks. Fragment shader assembler adds +2 weeks.

### Rust NID/FNID tooling

Under `tools/nidgen/` (Rust workspace, `Cargo.toml`):

```
tools/nidgen/
├── src/
│   ├── main.rs          # CLI: nidgen {nid|stub|verify|coverage}
│   ├── nid.rs           # SHA-1(symbol || suffix)[0..4], little-endian swap
│   ├── db.rs            # serde_yaml loader for NID YAML
│   ├── stubgen.rs       # emit .S stubs per exported symbol
│   ├── archive.rs       # drive ppu-ar / spu-ar to build .a stubs
│   └── verify.rs        # cross-check against reference stub .a symbol tables
├── nids/
│   ├── schema.yaml      # JSON Schema
│   ├── sys_lv2.yaml
│   ├── cellSysutil.yaml, cellGcmSys.yaml, cellNetCtl.yaml, cellPad.yaml,
│   ├── cellAudio.yaml, cellSpurs.yaml, cellFs.yaml, ...
└── tests/fnid-vectors.rs   # (name, fnid) pairs from reference stub .a
```

FNID constant (confirmed from user spec and PSDevWiki):

```
ps3_nid_suffix = [0x67,0x59,0x65,0x99,0x04,0x25,0x04,0x90,
                  0x56,0x64,0x27,0x49,0x94,0x89,0x74,0x1A]
fnid = swap_bytes_u32( SHA-1(symbol || suffix)[0..4] )
```

Unit test against known FNIDs extracted from reference stub .a symbol tables (blocked until reference SDK path is mounted).

### YAML schema (example)

```yaml
library: cellNetCtl
version: 1
module: sys_net
exports:
  - name: cellNetCtlInit
    nid: 0xbd5a59fc
    signature: "int cellNetCtlInit(void)"
  - name: cellNetCtlGetInfo
    nid: 0x8ce13854
    signature: "int cellNetCtlGetInfo(int code, CellNetCtlInfo *info)"
```

### PSL1GHT v3 RFC naming migration

Implement Cell-style naming per Issue #67: functions `cellXxx`, types `CellXxx`, constants `CELL_XXX_*`, syscalls `sys_*` snake_case. Ship `PSL1GHT/ppu/include/psl1ght-compat.h` that aliases old names → new names; existing homebrew migrates via `sed` scripts documented in `docs/migration-from-psl1ght.md`. Keep the shim for 1–2 releases, then deprecate.

Target renames (non-exhaustive; exhaustive list built during PSL1GHT-vs-reference-SDK diff):

| Old PSL1GHT | New reference-compatible |
|---|---|
| `netCtlInfoGet` | `cellNetCtlGetInfo` |
| `NET_CTL_STATE_IPObtained` | `CELL_NET_CTL_STATE_IPObtained` |
| `padInit`, `padGetData` | `cellPadInit`, `cellPadGetData` |
| `sysUtilEnable` | `cellSysutilRegisterCallback` |
| `gcmInitBody` | `cellGcmInitBody` |
| `audioPortOpen` | `cellAudioPortOpen` |
| `sysFsOpen` | `cellFsOpen` |

### Coverage matrix tool (`tools/coverage-report/`)

Rust + `clang-sys`. Walks `reference/private/target/{ppu,spu}/include/` and `PSL1GHT/{ppu,spu}/include/`, extracts function declarations + struct names + constants, emits `docs/coverage.md` as a matrix (`missing`/`present`/`renamed`/`signature-mismatch`). Target: ≥95% coverage on `cellGcm*`, `cellNetCtl*`, `cellPad*`, `cellAudio*`, `cellSysutil*`, `cellFs*`, `cellSpurs*`, `sys_*`.

Coverage tool is blocked on reference SDK mount. Until then it runs in PSL1GHT-only mode (just dumps the current PSL1GHT surface).

### Fragment shader (NV40-FP assembler)

New files in PSL1GHT:

- `PSL1GHT/ppu/include/rsx/nv40_fp.h` — NV40 fragment program ISA opcodes/operand encoding
- `PSL1GHT/ppu/source/rsx/nv40_fp_assembler.c` — text → binary assembler
- `PSL1GHT/ppu/source/rsx/fragment_program.c` — VRAM upload + bind runtime: `cellGcmSetFragmentProgram`, `cellGcmSetFragmentProgramParameter`
- `samples/rsx-textured-quad-fs/` — end-to-end validation sample

Reference sources: Nouveau `nv40_fragprog.c` (Mesa), public Nvidia `NV_fragment_program` extension docs, Cell OS Lv-2 manual. Clean-room reimplementation, no proprietary code copied.

### Networking gap fill

- `getaddrinfo` wrapping `sys_net_resolver_*` (~1 week)
- `select` / `poll` completion against `sys_net_bnet_select` (~3 days)
- TLS punted to Phase 4 (mbedTLS portlib)

### Critical files (Phase 3)

- `tools\nidgen\src\nid.rs` (load-bearing FNID algorithm)
- `tools\nidgen\nids\*.yaml` (NID database — populated from reference stub .a via `nidgen verify` after SDK mount)
- `src\ps3dev\PSL1GHT\ppu\include\psl1ght-compat.h` (new compat shim)
- `src\ps3dev\PSL1GHT\ppu\source\rsx\nv40_fp_assembler.c` (fragment shader enablement)
- `tools\coverage-report\src\main.rs`

---

## Phase 4 — ps3libraries / portlibs

**Effort:** Medium. **Duration:** 3–5 weeks. Parallel with Phases 3 and 5.

Rebuild common libraries against the new toolchain and bump versions:

| Library | Target version |
|---|---|
| zlib | 1.3.1 |
| libpng | 1.6.43 |
| libjpeg-turbo | 3.0.3 |
| freetype | 2.13.2 |
| libxmp | 4.6.0 |
| SDL2 | 2.30.x (new — SDL2 vs SDL1) |
| libcurl | 8.9.x |
| mbedTLS | 3.6.0 (new — unlocks HTTPS) |
| libssh2 | 1.11.0 |
| libzip | 1.10.x (new) |
| mpg123 | 1.32.x |
| FLAC | 1.4.3 |
| libogg / libvorbis / libtheora | unchanged (stable) |

`scripts/build-portlibs.sh` orchestrates per-library recipes under `scripts/portlibs/<libname>.sh`: fetch tarball, verify SHA-256, apply `patches/portlibs/<libname>/*.patch`, configure with `ppu-gcc -mcpu=cell -mlongcall -fPIC`, install to `$PS3DEV/portlibs/ppu/`. Emit `.pc` files. SDL2 needs custom backends (`cellAudio` for audio, `RSX` for video).

### Critical files

- `scripts\build-portlibs.sh`
- `scripts\portlibs\SDL2.sh` (non-trivial — custom audio/video backends)
- `scripts\portlibs\mbedtls.sh`, `curl.sh`
- `portlibs-manifest.yaml` (reproducibility manifest)

---

## Phase 5 — Infrastructure

**Effort:** Medium. **Duration:** 3–5 weeks. Parallel with Phases 3 and 4.

### Superbuild: CMake 3.27+ with ExternalProject_Add

`CMakeLists.txt` at repo root drives all subprojects. Modules under `cmake/`: `BuildPpuBinutils.cmake`, `BuildPpuGcc.cmake`, `BuildPpuNewlib.cmake`, `BuildSpuBinutils.cmake`, `BuildSpuGcc.cmake`, `BuildSpuNewlib.cmake`, `BuildPsl1ght.cmake`, `BuildPortlibs.cmake`, `Helpers.cmake`. CMake works natively on MSYS2 MinGW64, supports partial rebuilds, tracks cross-phase dependencies.

Invocation: `cmake -B build -S . -G Ninja -DPS3DEV=C:/Users/FirebirdTA01/source/repos/PS3_Custom_Toolchain/stage/ps3dev && cmake --build build`.

### Docker images for reproducibility

- `ci/docker/linux.Dockerfile` — `ubuntu:22.04` base, full toolchain build, publishes `ghcr.io/<user>/ps3-custom-toolchain:linux`. Even though user ships native MSYS2, Linux Docker makes CI cheap and gives users a pre-built alternative.
- `ci/docker/msys2.Dockerfile` — best-effort Windows Docker (optional).

### GitHub Actions CI

- `ci/github-actions/build-linux.yml` — matrix `{ppu-gcc: [12.4.0], spu-gcc: [9.5.0]}`, cache `build/`, build Phases 1 + 2, smoke-test Phase 3, upload `stage/ppu.tar.zst` + `stage/spu.tar.zst`.
- `ci/github-actions/build-msys2.yml` — MSYS2 MinGW64, lower priority.
- `ci/github-actions/coverage.yml` — runs `tools/coverage-report` on every PR, posts delta as comment. Blocked on reference SDK mount being available in a secure CI context (or runs PSL1GHT-only until then).

### Documentation

- `docs/quickstart.md` — zero to hello-world on RPCS3 in 10 minutes (pre-built toolchain path)
- `docs/migration-from-psl1ght.md` — rename table, sed scripts, compat header usage
- `docs/abi-reference.md` — NID/FNID algorithm, SELF/SPRX format, TLS layout, module init/exit
- `docs/spu-programming.md` — LS budget, DMA patterns, intrinsic use
- `docs/rsx-programming.md` — vertex pipeline (existing) + fragment pipeline (new)
- `docs/coverage.md` — auto-generated

### Critical files

- `CMakeLists.txt` (top-level superbuild)
- `cmake\BuildPpuGcc.cmake`, `BuildSpuGcc.cmake` (load-bearing)
- `ci\github-actions\build-linux.yml`
- `ci\docker\linux.Dockerfile`

---

## Phase 6 — Validation & Milestones

**Effort:** Medium. **Duration:** 2–4 weeks concentrated + ongoing.

### Milestone sequence

| Milestone | Deliverables | Blockers |
|---|---|---|
| M0 Bootstrap | Phase 0 complete | — |
| M1 PPU Alpha | Hello-world C++17 on PPU runs on RPCS3 | M0 |
| M2 SPU Alpha | hello-spu runs in RPCS3 SPU interpreter | M0 |
| M3 Joint Alpha | PPU+SPU DMA mailbox exchange works | M1, M2 |
| M4 PSL1GHT Beta | v3 naming, NID tooling, networking complete | M3, reference SDK mount |
| M5 Fragment Shader | NV40-FP assembler + textured-quad sample renders | M4 |
| M6 Portlibs Beta | All portlibs v1 ship; SDL2 demo runs | M4 |
| M7 CI Green | Phase 5 complete | M6 |
| M8 Feature Complete | ≥95% reference-SDK API coverage + 10 samples run on hardware + RPCS3 | M7 |
| M9 Phase 2b (parallel) | SPU backend on GCC 12.4.0 | independent |

### "Feature complete" definition

1. ≥95% of `cellGcm*`, `cellNetCtl*`, `cellPad*`, `cellAudio*`, `cellSysutil*`, `cellFs*`, `cellSpurs*`, `sys_*` present with matching signatures.
2. 10 curated samples build with zero warnings, run on both RPCS3 and real hardware.
3. `-std=c++17` on both PPU and SPU. `-std=c++20` on PPU.
4. GDB remote works against RPCS3 PPU gdbstub.
5. quickstart + migration + ABI reference docs complete.
6. CI green on Linux; MSYS2 manual-validated; Docker image published.
7. RSX vertex + fragment pipelines functional — textured lit rotating cube sample renders correctly.

---

## Open Items / Deferred

- **reference SDK mount path.** User provides when ready. `scripts/bootstrap.sh` will document the `mklink /J` junction command. Phase 3 coverage tooling + stub verification blocks on this; until then it runs in PSL1GHT-only mode.
- **Phase 2b schedule.** Parallel/long-lead; no hard date. Completes at M9, which may be 6+ months after M8.
- **Full GLSL/Cg fragment shader compiler.** Follow-up to NV40-FP assembler. Not in this plan.
- **Public vs private git remote.** Assume fully public OSS; revisit if any proprietary-reference-adjacent artifacts need to stay private.

---

## Verification (end-to-end)

After execution, run in this order:

1. `scripts/bootstrap.sh` — clones repos, installs MSYS2 deps, scaffolds directories. Expect zero errors.
2. `scripts/build-ppu-toolchain.sh` → produces `$PS3DEV/bin/powerpc64-ps3-elf-gcc`. Run `powerpc64-ps3-elf-gcc --version` (expect 12.4.0). Build `samples/hello-ppu-c++17/` → `.self`. Load in RPCS3; verify `printf` output in log.
3. `scripts/build-spu-toolchain.sh` → produces `$PS3DEV/bin/spu-elf-gcc` (9.5.0). Build `samples/hello-spu/`. Load from PPU host via `sys_spu_image_import`; verify mailbox printf reaches PPU.
4. `scripts/build-psl1ght.sh` → PSL1GHT with v3 naming + compat shim + NV40-FP assembler. Build `samples/ppu-spu-dma/` and `samples/rsx-triangle-vs/` — both run in RPCS3.
5. `cargo test -p nidgen` → FNID vectors pass.
6. `tools/coverage-report` → emits `docs/coverage.md`; manually inspect — Phase 4 ship gate is ≥95% on named subsystems (awaits reference SDK mount).
7. `cmake --build build` from repo root — full superbuild green.
8. `samples/rsx-textured-quad-fs/` builds and renders on RPCS3 (M5).
9. Real-hardware run: deploy 10 samples as PKG to jailbroken PS3; each boots and behaves correctly (M8).
10. GDB remote: `powerpc64-ps3-elf-gdb` connects to RPCS3 gdbstub, sets breakpoint in PPU code, single-steps.

---

## Effort Summary

| Phase | Effort | Weeks |
|---|---|---|
| 0 Bootstrap | Low | 0.5–1 |
| 1 PPU GCC 12.4.0 | High | 4–8 |
| 2a SPU GCC 9.5.0 | Medium | 3–5 |
| 2b SPU forward-port | Very High | 12–24 (parallel) |
| 3 PSL1GHT v3 + FP assembler | High | 6–10 + 2 |
| 4 portlibs | Medium | 3–5 |
| 5 Infrastructure | Medium | 3–5 |
| 6 Validation | Medium | 2–4 |

**Minimum viable ship (M0–M8, excl. 2b):** ~5–7 months focused work.
**Full feature complete incl. 2b:** ~12–18 months.
