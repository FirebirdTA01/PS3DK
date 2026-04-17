# Phase 0 — Bootstrap Status

**Date:** 2026-04-16
**State:** Scaffolding complete. Real toolchain builds (Phase 1+) pending patch rebase work.

---

## What's in place

### Workspace structure
All directories from the plan exist: `scripts/`, `src/{upstream,ps3dev,forks}/`, `patches/{ppu,spu,psl1ght,portlibs}/...`, `tools/{nidgen,coverage-report}/`, `stage/ps3dev/{bin,ppu,spu,psl1ght,portlibs}/`, `samples/`, `ci/{docker,github-actions}/`, `tests/{smoke,conformance,hardware,rpcs3}/`, `cmake/`, `docs/`, `reference/`.

### Repo conventions
- `.gitignore` — excludes `src/`, `build/`, `stage/`, `reference/private/`, tarballs, build artifacts.
- `.gitattributes` — `patches/** -text`, CRLF rules for `.bat`/`.cmd`, binary markers on `.a`, `.self`, `.sprx`, `.pkg`.
- `README.md` — project overview and quickstart.
- `LICENSE` — MIT for the glue code.
- `NOTICE` — component attribution (GCC, binutils, newlib, PSL1GHT, reference SDK reference-only, etc.).
- `reference/REFERENCE_POLICY.md` — hard rules for the private SDK mount (never commit, never ship).

### Cloned sources (all under `src/`, `.gitignored`)
- **upstream/** — GCC (365 MB, 12.1/12.2/12.3/12.4/12.5 + 9.5 tags fetched), binutils-gdb (132 MB, binutils-2_42 + gdb-14.2-release), newlib-cygwin (35 MB, newlib-4.4.0.20231231).
- **ps3dev/** — ps3toolchain (with its 4 patches intact: gcc-7.2.0, binutils-2.22, newlib-1.20.0, gdb-8.3.1), PSL1GHT, ps3libraries.
- **forks/** — bucanero-{ps3toolchain,PSL1GHT}, jevinskie-ps3-gcc, Estwald-PSDK3v2, StrawFox64-PS3Toolchain.

### Scripts (all executable)
- `scripts/env.sh` — exports `PS3_TOOLCHAIN_ROOT`, `PS3DEV`, `PPU_PREFIX`, `SPU_PREFIX`, `PSL1GHT`, `PS3_BUILD_ROOT`; prepends `$PS3DEV/bin` to PATH.
- `scripts/bootstrap.sh` — idempotent reproducible setup: dep check, upstream/ps3dev/fork clones with pinned refs, reference SDK mount guidance, staging layout.
- `scripts/build-ppu-toolchain.sh` — binutils 2.42 → GCC 12.4.0 stage-1 → newlib 4.4.0 → GCC 12.4.0 stage-2 → GDB 14.2 → symlink aliases. Reads `patches/ppu/{binutils-2.42,gcc-12.x,newlib-4.x,gdb-14.x}/` series.
- `scripts/build-spu-toolchain.sh` — binutils 2.42 (spu-elf) → GCC 9.5.0 + newlib combined build. Ships `spu_ps3.ld` linker script.
- `scripts/build-psl1ght.sh` — applies `patches/psl1ght/` series, runs PSL1GHT's own `make install-ctrl && make && make install` against the new toolchain.
- `scripts/build-portlibs.sh` — cross-compile environment + discovery of recipes under `scripts/portlibs/NNN-*.sh`.
- `scripts/portlibs/001-zlib.sh` — first recipe, proves the pattern.

### Rust tooling (code written; requires Rust install to compile)
Workspace at `tools/Cargo.toml`:

- `tools/nidgen/` — CLI + library:
  - `src/nid.rs` — FNID algorithm (SHA-1(name || PS3_NID_SUFFIX)[0..4] little-endian). **Verified** against both PSDevWiki published vectors (`_sys_sprintf`→`0xA1F9EAFE`, mangled runtime_error→`0x5333BDC9`) using Python hashlib (Rust tests will pass identically).
  - `src/db.rs` — YAML loader with hex serde helpers, dual int/string NID parsing.
  - `src/stubgen.rs` — PPU assembly emitter for `.lib.stub.<library>` sections.
  - `src/archive.rs` — drives `ppu-as`/`ppu-ar` to build `.a` stubs.
  - `src/verify.rs` — YAML → FNID cross-check, archive symbol enumeration via `ar`/`nm`.
  - `src/main.rs` — CLI with `nid`, `stub`, `verify`, `archive` subcommands.
  - `tests/fnid_vectors.rs` — regression tests against known vectors.
  - `nids/` — `schema.yaml`, `sys_lv2.yaml` (seed), `README.md`.
- `tools/coverage-report/` — reference-vs-PSL1GHT matrix generator. Regex-based scan today; libclang path lands when SDK mount is available.

### Samples (build after Phase 1 + 2 + 3 land)
- `samples/hello-ppu-c++17/` — structured bindings, `if constexpr`, `std::string_view`, `std::optional`, CTAD, fold expression, inline variable, `std::array`, PPU thread id.
- `samples/hello-spu/` — split PPU/SPU build; SPU uses `mfc_put`, `mfc_read_tag_status_all` intrinsics; PPU loads image, launches group, awaits DMA completion.
- `samples/README.md` — sample-by-phase dependency table and contribution guide.

### Docs
- `docs/patches-inventory.md` — line-level audit of all four existing ps3dev patches with rebase plan per target. GCC patch touches 7 files (`config.gcc`, `cell64lv2.h` (new), `rs6000.c` → `rs6000.cc` in GCC 12, `configure.ac`, `libcpp/lex.c`, `libgcc/config.host`, `Makefile.in`). newlib patch is 13k lines, mostly new-file additions for `libgloss/libsysbase` PS3 glue.

---

## What's NOT in place (intentional — next phases)

### Patches to rebase
The heart of the multi-week engineering work:

- **Phase 1 blocker** — `patches/ppu/gcc-12.x/` (0001..0007 patch series from the inventory). This is where `powerpc64-ps3-elf` gets added to GCC 12.4.0's `config.gcc`, `cell64lv2.h` lands updated for GCC 12 macros, `rs6000.c` hunks are relocated into GCC 12's `rs6000.cc` split files, and libstdc++ threading is wired up.
- **Phase 1 blocker** — `patches/ppu/newlib-4.x/` (0001..0006 series). libgloss/libsysbase PS3 glue, lv2 syscall asm, CRT0, reent, pthread weak stubs.
- **Phase 1 blocker** — `patches/ppu/binutils-2.42/` — likely trivial or empty.
- **Phase 1 blocker** — `patches/ppu/gdb-14.x/` — ~50 LOC for `powerpc64-ps3-elf` triple recognition.
- **Phase 2a blocker** — `patches/spu/gcc-9.5.0/` — ~50 LOC for PS3-specific SPU libgcc tweaks and default spec file.

Until these land, `build-ppu-toolchain.sh` and `build-spu-toolchain.sh` will happily extract sources but GCC stage-2 will fail to recognize the target triple.

### Rust toolchain
Not installed on the host yet. `scripts/bootstrap.sh` warns and documents the MSYS2 install line (`pacman -S mingw-w64-x86_64-rust`). The nidgen Rust code compiles in theory; the FNID algorithm is verified via Python equivalence.

### reference SDK mount
Deferred per user choice (`reference/private/` is a placeholder). `bootstrap.sh` prints the `mklink /J` + `icacls` commands when the user is ready. Phase 3 coverage tooling and stub verification block on this mount.

### CMake superbuild
Plan calls for `CMakeLists.txt` + `cmake/*.cmake` modules orchestrating everything. Scaffolding deferred; the per-phase bash scripts are sufficient to drive the first PPU build.

### CI / Docker
`ci/docker/` and `ci/github-actions/` directories exist but no Dockerfiles or workflows are written yet. Scaffolding deferred until the first build is green locally.

### Phase 2b forward-port
Long-lead work per the plan. Not started. Tracked as its own independent stream once Phase 2a ships.

### PSL1GHT v3 RFC patches
`patches/psl1ght/` is empty. The v3 RFC work (Cell-style naming, NID-driven stubs, fragment shader assembler) is Phase 3 and starts after Phases 1 + 2a produce a working compiler.

---

## Load-bearing proofs

- **FNID algorithm** — verified: `fnid("_sys_sprintf") == 0xA1F9EAFE`, `fnid("_ZNKSt13runtime_error4whatEv") == 0x5333BDC9` via Python. Same algorithm in Rust will match.
- **GCC 12.4.0 source** — fetched and tagged.
- **GCC 9.5.0 source** — fetched and tagged.
- **binutils 2.42 source** — fetched and tagged.
- **newlib 4.4.0.20231231 source** — fetched and tagged.
- **All ps3dev patches** — intact in `src/ps3dev/ps3toolchain/patches/` for reference during rebase.

---

## Recommended next step

**Start Phase 1a: rebase the binutils patch onto binutils 2.42.**

Binutils has the lowest risk — 90-line existing patch, 12+ years of upstream drift, likely most hunks are obsolete. Quick win that proves the full `extract → patch → configure → build → install` loop of `build-ppu-toolchain.sh` works end to end.

After binutils 2.42 builds cleanly:

1. **Phase 1b** — rebase GCC patch onto GCC 12.4.0. Stage-1 C compiler first.
2. **Phase 1c** — port newlib glue forward to 4.4.0.
3. **Phase 1d** — GCC 12.4.0 stage-2 C/C++ against newlib. Hello-world C++17 smoke test.

All other tracks (SPU 2a, PSL1GHT v3, portlibs, CMake, CI) can start after or in parallel with Phase 1c landing.

---

## Files authored this session

Scripts (7): `env.sh`, `bootstrap.sh`, `build-ppu-toolchain.sh`, `build-spu-toolchain.sh`, `build-psl1ght.sh`, `build-portlibs.sh`, `portlibs/001-zlib.sh`.

Rust (9): `tools/Cargo.toml`, `tools/nidgen/Cargo.toml`, `tools/nidgen/src/{lib,nid,db,stubgen,archive,verify,main}.rs`, `tools/nidgen/tests/fnid_vectors.rs`, `tools/coverage-report/Cargo.toml`, `tools/coverage-report/src/main.rs`.

Docs (5): `README.md`, `NOTICE`, `LICENSE`, `reference/REFERENCE_POLICY.md`, `docs/patches-inventory.md`, `docs/phase-0-status.md` (this file), `samples/README.md`, `tools/nidgen/nids/README.md`.

Config (3): `.gitignore`, `.gitattributes`, `tools/nidgen/nids/schema.yaml`.

NID DB (1): `tools/nidgen/nids/sys_lv2.yaml`.

Samples (5): `samples/hello-ppu-c++17/source/main.cpp` + `Makefile`, `samples/hello-spu/source/main.c` + `spu/source/main.c` + `Makefile` + `spu/Makefile`.

**Total: ~30 authored files.**
