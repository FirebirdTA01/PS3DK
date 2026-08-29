# Sample build sweep — v0.10.715 (baseline)

**Date:** 2026-08-28 · **Package:** `ps3-sdk-v0.10.715-windows-x86_64`
**Built from:** the extracted release zip on a Windows host, not the source tree —
i.e. exactly what a user gets. Tree at `4e74007` (+ a docs-only commit).
**Reproduce with:** `scripts/sweep-samples.ps1 <extracted-package-root>`

This is the **baseline** for the librt fixes (t_cd49e350). Re-run after they land
and diff, so "did it help" is measured rather than argued.

## Result: 187 / 196 on a raw extract — 192 / 196 after the documented first run

Two numbers, because they mean different things. **187/196** is what a user gets
from a freshly extracted zip. **192/196** is what they get after running
`setup.cmd`, which builds the `pkgcrypt` Python extension. The 5-sample gap is
not a broken SDK; it is an undeclared first-run dependency (Cause A below).
The remaining 4 failures are real defects and are unaffected by first run.

| Family | Pass | Total | | Family | Pass | Total |
|---|---|---|---|---|---|---|
| PSL1GHT | 20 | 21 | | lv2 | 9 | 9 |
| audio | 4 | 4 | | network | 13 | 13 |
| codec | 14 | 14 | | spu | 4 | 4 |
| dbgfont | 1 | 1 | | spurs | 12 | 12 |
| font | 1 | 1 | | sysutil | 38 | 39 |
| fw | 4 | 4 | | toolchain | 46 | 50 |
| gcm | 20 | 23 | | vision | 1 | 1 |

## The 9 failures — three distinct causes, none of them the compiler

### A. Missing `pkgcrypt` — 5 samples (first-run defect)
`hello-ppu-cellgcm-global-uniforms`, `hello-ppu-cellgcm-sysmenu`,
`hello-ppu-cellgcm-textured-cube`, `audiotest`, `hello-ppu-gamecontent`

    ModuleNotFoundError: No module named 'pkgcrypt'

Every one calls `ps3_add_pkg`, which runs `pkg.py`, which imports `pkgcrypt` — a
Python C extension the package ships as **source** (`crypt.c`) and builds at first
run via `setup.cmd`. On a fresh extract where `setup.cmd` has not been run, these
five fail. Nothing is wrong with the toolchain; the package has a build-time
dependency on the user's Python that is neither declared nor validated.
**Verified, not inferred:** the extract had no `pkgcrypt*.pyd` in `bin/`. Running
`build_pkgcrypt.py` (exactly what `setup.cmd` does) and rebuilding these five
gives **5/5 PASS**. So the diagnosis is confirmed end to end: these samples are
not broken, the package simply has a build-time dependency on the user's Python
that is neither declared nor validated, and nothing fails until a user builds one
of the five samples that calls `ps3_add_pkg`.

This is the measured cost of the P5 item (`t_3b2b85e3`, adopt the pure-C sfo/pkg
tools): **5 samples, 2.6% of the suite, broken until a first-run step nobody is
told is mandatory.**

### B. Files that exist in the repo but are not staged — 3 samples (packaging gap)
| Sample | Needs | In repo? | In package? |
|---|---|---|---|
| `hello-psgl-spudraw` | `PSGL/spu_psgl.h` | yes — `sdk/include-spu/PSGL/` | **no** |
| `hello-psgl-spudraw-triangle` | `PSGL/spu_psgl.h` | yes — `sdk/include-spu/PSGL/` | **no** |
| `hello-psgl-glsl-object` | `tools/psgl/glsl_subset_to_cg.py` | yes — `tools/psgl/` | **no** |

`package-windows-release.sh` stages `ppu/`, `spu/`, `$PS3DK`, `portlibs/`,
`samples/`, `bin/`, `sdk/assets/`, `cmake/`, `scripts/version.sh` and
`sdk/include` — but **not** `sdk/include-spu/` or `tools/`. Both directories exist
and are needed by shipped samples.

Note this is a gap the required-artifact manifest cannot currently catch: it
validates libraries, host tools and stub aliases, not sample **support files**.
The manifest's two-sided check proves every shipped *archive* is accounted for; it
says nothing about a header or script that was never staged at all.

### C. Requires a host C compiler — 1 sample (Windows-hostile assumption)
`hello-psgl-ffp-shaderlib` — `CMakeLists.txt:27` does
`find_program(PS3_HOST_CC NAMES cc gcc clang REQUIRED)`. A stock Windows box has
none of those on PATH, so configure fails before anything is built. Not a
packaging bug: the sample assumes a Unix-like host toolchain.

## What this sweep does NOT tell you

Building is not running. 187 samples link; **none of that exercises the two
defects we know ship in this package** — the time wrappers and `socket()`
returning `ENOSYS` for every CMake-built program. The network family is 13/13
green here and its sockets are non-functional at runtime. That is Stage 2.
