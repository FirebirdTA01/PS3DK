# PAL selection guard: planned preprocess checks

These are PLANNED checks, for review only. Nothing here was run, and nothing is wired into a build, CI job or test
list. No compile or run GO is implied (codex 2026-10-22 03:21).

## The guard

`sdk/libps3dkprof/src/pal/pal.h` is the SHARED header, so the core, the drain and the PAL all go through it. It
refuses three configurations with `#error`:
- both PAL macros defined;
- neither defined;
- `PS3DK_PROF_PAL_LV2` together with `PS3DK_PROF_TEST_HOOKS`.

## Planned cases

Each case preprocesses `pal_select.c` only (`cc -E`), with no other flag beyond the include path the request names.

| Case | Defines | Expected |
|---|---|---|
| both | `-DPS3DK_PROF_PAL_HOST -DPS3DK_PROF_PAL_LV2` | nonzero rc; stderr contains `define exactly one of PS3DK_PROF_PAL_HOST and PS3DK_PROF_PAL_LV2, not both` |
| neither | (none) | nonzero rc; stderr contains `define exactly one of PS3DK_PROF_PAL_HOST and PS3DK_PROF_PAL_LV2` and NOT `not both` |
| lv2-hooks | `-DPS3DK_PROF_PAL_LV2 -DPS3DK_PROF_TEST_HOOKS` | nonzero rc; stderr contains `PS3DK_PROF_TEST_HOOKS is host-only` |
| host (positive control) | `-DPS3DK_PROF_PAL_HOST` | rc 0; no `#error` text; output contains `pal_select_probe_marker` |
| lv2 (positive control) | `-DPS3DK_PROF_PAL_LV2` | rc 0 with the TARGET compiler and sysroot (it includes the target `<pthread.h>`); no `#error` text; output contains `pal_select_probe_marker` |

- **The host cases** (both, neither, lv2-hooks, host) can run with the host compiler. In the both and lv2-hooks
  cases, every expected message comes from a guard placed before any platform include.
- **The lv2 positive control** needs the GCC 16 target compiler and the installed headers. Their identities are
  pinned by the request.
- **How results are judged:** each case keeps its argv, rc, stdout and stderr. A message is matched exactly as a
  substring, with its grep status captured (0 found, 1 absent, greater than 1 inspection failure). A nonzero rc
  WITHOUT the expected message is a FAIL, not a pass.

## What these checks do not prove

The guard stops a translation unit that sees a contradictory or missing selection. It does not prove that every
object in a build used the SAME branch: a build could compile some objects with HOST and others with LV2. The frozen
build request must therefore bind every core, drain and PAL compile invocation, and the installed header and
provider identities.
