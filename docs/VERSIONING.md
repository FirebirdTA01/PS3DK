# Versioning

This repo ships a multi-component SDK (PPU/SPU toolchains, runtime
libraries, headers, and tools written in Rust + C++).  All of those
components share a single version string sourced from the most recent
git tag.

## TL;DR

- The canonical version is the most recent annotated tag matching
  `vMAJOR.MINOR.PATCH`.
- Cutting a release is `git tag vX.Y.Z && git push --tags`.  GitHub
  Actions does the rest.
- For day-to-day builds, untagged commits get monotonically increasing
  patch numbers (`vX.Y.<Z+commits-since-tag>`).
- A dirty working tree appends `+dirty` to human-readable strings.

## Source of truth: `scripts/version.sh`

One generator feeds every consumer.  Run it manually any time:

```sh
$ ./scripts/version.sh --format=plain
v0.1.0
$ ./scripts/version.sh --format=bare
0.1.0
$ ./scripts/version.sh --format=header   # C #defines for sdk_version.h
$ ./scripts/version.sh --format=cmake    # set(...) lines for include()
$ ./scripts/version.sh --format=json     # for tooling
```

Logic:

1. Find the most recent tag matching `v[0-9]+.[0-9]+.[0-9]+`.
2. If `HEAD` is that tag exactly, use it verbatim.
3. Otherwise add `git rev-list <tag>..HEAD --count` to the patch number.
4. If no tag exists yet, fall back to `v0.0.0` and use total commit count
   as the patch.
5. If the working tree is dirty, append `+dirty` to the *display* string.
   Numeric components are unaffected.

## Consumers

| Consumer | How it picks up the version |
|---|---|
| **SDK runtime headers** (`sdk/Makefile install`) | `scripts/version.sh --format=header > $PS3DK/ppu/include/cell/sdk_version.h`.  `cell/cell.h` includes it; user code reaches the version via `PS3SDK_VERSION` and friends. |
| **SDK install marker** | `$PS3DK/VERSION` plain-text file written at install time.  Useful for shell scripts and tarball naming. |
| **Rust workspace** (`tools/Cargo.toml`) | `[workspace.package] version` is the canonical Rust-side value.  `scripts/sync-versions.sh` rewrites it from `version.sh --format=bare` idempotently.  Each Rust binary picks it up via `CARGO_PKG_VERSION` (clap's `version` attribute exposes `--version` automatically). |
| **rsx-cg-compiler** (CMake) | `tools/rsx-cg-compiler/CMakeLists.txt` calls `version.sh --format=cmake` at configure time, includes the result, and `configure_file`s `src/version.h.in` → `<build>/version.h`.  `main.cpp` includes the generated header for `RSX_CG_COMPILER_VERSION`, exposed via `--version` / `-V`. |
| **Toolchain prefix tarball** (post-v0.3.0) | Will name itself `ps3-sdk-vX.Y.Z-<host>-<arch>.tar.zst` using `version.sh --format=plain`. |

## Cutting a release

1. **Update the changelog.**  Move items from `## [Unreleased]` into a
   new `## [vX.Y.Z] — YYYY-MM-DD` section in `CHANGELOG.md`.  Follow
   [Keep a Changelog](https://keepachangelog.com/) conventions.

2. **Sync the Rust workspace version.**

   ```sh
   ./scripts/sync-versions.sh --dry-run   # preview
   ./scripts/sync-versions.sh             # write
   git add tools/Cargo.toml CHANGELOG.md
   git commit -m "release: vX.Y.Z"
   ```

3. **Tag and push.**

   ```sh
   git tag -a vX.Y.Z -m "vX.Y.Z"
   git push origin main
   git push origin vX.Y.Z
   ```

4. **GitHub Actions takes over.**  `.github/workflows/release.yml`
   verifies `version.sh` matches the tag, builds Rust tool binaries on
   Linux x86_64, packages a curated source tarball, extracts the matching
   CHANGELOG section, and creates a GitHub Release with the assets
   attached.  Watch the run; if anything fails, fix forward (delete the
   tag, fix, retag) — releases are cheap.

## Versioning scheme

Pre-1.0 while we're bootstrapping the SDK.

- **Minor (`0.x.0`)** — bumps on phase completion or significant SDK
  surface additions.  May contain breaking changes; the changelog is the
  contract.
- **Patch (`0.x.y`)** — bug fixes and additive non-breaking changes
  within a minor line.
- **`v1.0.0`** — when:
  - `hello-ppu-c++17`, `hello-spurs-task`, the reference `basic.cpp`,
    and `5spu_spurs_without_context` all build and run clean against
    a fresh install of the published tarball, on a clean machine,
    without manual fixups;
  - the `cell::Gcm::` Sony-name surface is complete;
  - the public `cell/*` and `sys/*` header surfaces are frozen;
  - the SDK is verified on real PS3 hardware (not just RPCS3).

Versions before that point are pre-release in spirit even though they
don't carry SemVer pre-release suffixes — homebrew authors should pin
exact tags, not `v0.x` ranges, until 1.0.

## Release-payload roadmap

The long-term goal is **out-of-the-box homebrew development** from a
single tarball: download, extract, write code, build, run.  We get there
in stages.

### v0.1.x — what's shipping today

- `ps3-sdk-src-vX.Y.Z.tar.xz` — curated source tree (`git archive`).
- `ps3-sdk-tools-vX.Y.Z-linux-x86_64.tar.xz` — host tool binaries
  (`nidgen`, `abi-verify`, `coverage-report`, `sprxlinker`,
  `rsx-cg-compiler`), dynamic-linked against the system libelf/zlib/zstd.
- `ps3-sdk-tools-vX.Y.Z-windows-x86_64.zip` — the same binaries built
  via MSYS2 mingw64 with `-static -static-libgcc -static-libstdc++`,
  so each `.exe` is self-contained (no DLL runtime dependencies beyond
  what every Windows install ships).

The user still runs `scripts/bootstrap.sh` + the four `build-*-toolchain`
/ `build-sdk` scripts to produce the actual `$PS3DEV` tree.  First
bootstrap is a multi-hour build; subsequent rebuilds are fast.

### v0.2.x — Linux SDK tarball (target)

Add a third release asset:

- `ps3-sdk-runtime-vX.Y.Z-linux-x86_64.tar.xz` — the contents of
  `$PS3DK/` (our SDK headers and libs) plus a `setup.sh` that drops them
  next to an existing `$PS3DEV` install.  Lets a homebrew dev who already
  has a working PPU/SPU toolchain skip rebuilding our SDK on every
  release.

Cost: medium.  CI needs a prebuilt PPU+SPU toolchain to compile our
runtime libs against.  Approach: bootstrap the toolchain once on a
self-hosted runner (or in a one-shot GitHub Actions job with `ccache` +
`actions/cache`), upload the resulting `$PS3DEV` tarball as a
**non-released** workflow artifact, then have the per-release jobs pull
it as a build cache.

### v0.3.0 — full Linux + Windows toolchain tarballs (target)

Bundle everything under `$PS3DEV` for both Linux x86_64 and Windows
x86_64 in the same release:

- `ps3-sdk-vX.Y.Z-linux-x86_64.tar.xz` — `$PS3DEV/{ppu,spu,psl1ght,
  ps3dk,bin,portlibs}` end-to-end, ready to `tar -xf` into `~/ps3sdk`,
  source `env.sh`, and start writing samples.
- `ps3-sdk-vX.Y.Z-windows-x86_64.zip` — same shape as the Linux
  tarball but with `.exe` toolchain binaries and a `setup.cmd` for
  `%PS3DEV%`.

The host *tools* (rsx-cg-compiler, sprx-linker, the Rust binaries) are
already shipping for Windows in v0.1.x.  What v0.3.0 adds is the
PPC/SPU **target compilers** themselves, which require a cross-build
from Linux to `x86_64-w64-mingw32` — binutils/GCC/newlib are bootstrapped
twice (once Linux→PPC, once Linux→MinGW), but each only once per release.

Cost: high build minutes (toolchains take hours to build from source,
and the Windows cross adds another 2-3h on top of the native build).
Likely needs a self-hosted runner or paid Actions tier.  Worth it once
the SDK surface is stable enough that releases don't churn weekly.

### v1.0.0 — verified out-of-the-box experience

- All three host tarballs (Linux + Windows, plus macOS if reasonable).
- A `samples/` validation build is part of the release workflow: every
  candidate release tarball is round-tripped through a clean container,
  used to build hello-ppu-c++17 + hello-spurs-task + the reference
  basic.cpp, and the resulting `.self`s are run under RPCS3 in CI.
- A test on real PS3 hardware before the tag is cut (this part is
  human-in-the-loop).

The `release.yml` workflow grows incrementally — each milestone is
additive, so users on older tarballs keep working while we add new ones.

## Pre-release tags

Pre-release suffixes (e.g. `v0.2.0-rc1`) are supported by the release
workflow's tag pattern but not yet used in practice.  When we start
shipping release candidates ahead of a major Phase landing, prefer:

- `vX.Y.Z-rcN` for late-cycle validation runs.
- `vX.Y.Z-betaN` for earlier feature-complete previews.

`scripts/version.sh` does **not** currently parse suffixes — adjust the
generator before relying on them programmatically.
