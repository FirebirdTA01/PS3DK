# One modern GCC for PPU and SPU

Status: approved by the director on 2026-10-14 (GCC 16, the stage order below, one
compiler per release).  With the switchover change the PPU compiler is GCC
16.1.0 (stage 2, below); the SPU compiler is still GCC 9.5.0, the last release
with the SPE backend.  This plan moves
both to one current GCC release line and ships the new compiler only after it
passes every release gate.  The plan first meant to keep the ABI unchanged; the
C1 change (signed plain `char` under ILP32, 16-bit `wchar_t`, `int` `wint_t`,
LV2 `errno` values) changed it, so every object and archive must be rebuilt, C
and C++, PPU and SPU.

## Target version: GCC 16

| Line | Latest | State |
|---|---|---|
| 16 | 16.2 (2026-08-07) | newest stable; regression fixes only |
| 15 | 15.3 (2026-06-12) | stable |
| 14 | 14.4 (2026-06-26) | stable |
| 13 | 13.5 (2026-09-11) | oldest open branch, closes next |
| 12 | 12.5 | closed |
| 17 | trunk | general development |

GCC 16 is the newest stable line, so it has the longest supported life, and it
is the line both compilers can share: a forward-ported SPU backend has to be
written against one GCC's internal APIs anyway, and porting it twice (to 15,
then to 16) buys nothing.  Each stage starts from the newest 16.x point release
of the day and records it.

## Principle: never move two variables at once

Every stage changes one thing and is gated on its own: the pointer model, the
C sources, the PPU compiler version, the SPU compiler version.  A regression
then has one suspect.  Each stage lands only after the full release gate
(`docs/RELEASING.md`), plus the compiler checks below.

## Stage 0: the pointer model on GCC 12.4

Under ILP32, `Pmode` was SImode until v0.23.0 (patch 0005), so GCC treated a
pointer's upper 32 bits as don't-care, while the PPU forms effective addresses
from all 64 bits.  Twenty-three patches worked around the consequences one bug
at a time:
the `Pptr` iterator rewrites of `rs6000.md` (0012, 0015, about 900 diff lines),
middle-end canonicalisation in `convert.cc` and `expr.cc` (0029, 0030, 0032,
0033, 0055), DImode saves of callee-saved, LR and EH registers (0031, 0037),
the `-fno-ivopts` default (0049) and the doloop counter fix (0050).

Patch 0059 (developed as 0051 on `feat/ilp32-pmode-di`, card t_3b6c018a; shipped in
v0.23.0) changes the model instead: `Pmode` DImode, `ptr_mode` SImode, `POINTERS_EXTEND_UNSIGNED`.
Pointers are zero-extended where they become addresses, so a set bit 32 can
no longer reach the address unit, whichever path produced it.

Stage 0 landed the model (0059, retiring 0049) with its follow-ups 0060 to 0065:
TOC entry and slot widths (0060, 0062, 0063, 0065) and FP memory constraints
(0061, 0064).  It now retires every remaining workaround the new model
subsumes, one patch at a time, each removal proven by its regression row and
a codegen A/B against the full series: 0050, 0031, 0037, 0029, 0030, 0032, 0033, 0055, 0012 and 0015.
A patch the model does not subsume stays and is carried into the port.
Doing this before the version change matters for two reasons: the port carries
the small model instead of the workaround stack, and the model change is
validated on the compiler whose behaviour we already know.

Port map so far:

| Patch | Stage-0 result |
|---|---|
| 0050 | retired (0059 gives the doloop counter its full width) |
| 0031, 0037 | retired together (0037 builds on 0031); full-width callee, LR and EH-data saves come from 0059 |
| 0029 | **retained**: without it a direct pointer-to-64-bit conversion sign-extends under ILP32 (a semantic fix in `convert.cc`, not a Pmode workaround); guarded by `tests/sdk/ilp32-pointer-widen-codegen-test.sh` and the `ilp32-ptr-widen` row; a candidate for upstream submission |
| 0030 | **retained**: without it a signed `int` offset added to an ILP32 pointer is zero-extended (`lwz` where `lwa` belongs), and the vcall offset in virtual-base thunks likewise (the `ilp32-thunk-vcall` guard fails) |
| 0032, 0055 | retired together: both act only while Pmode is narrower than a word (0055 needs 0032's pattern), which 0059 ended; generated code identical, and `tests/abi/ppu-relative-switch-check.py` now expects the sign-extended dispatch in both data models |
| 0012, 0015 | retired together: the `Pptr` iterator selects the same modes as `P` once Pmode follows `TARGET_64BIT` (0059); 0057's two `rs6000.md` context lines are refreshed to the upstream `P` spelling |
| 0013, 0016-0019, 0028, 0057 | retired together: with 0059 and 0066, Pmode is DImode exactly when `TARGET_64BIT`, so their `Pmode` tests (prologue/epilogue and stack probes, TLS template variants and base register, `allocate_stack` residual, `STACK_SIZE_MODE`, indirect-call `ptrload`) pick upstream's branches, and 0057's `TARGET_64BIT && Pmode == SImode` TLS paths and patterns are unreachable; 0063's context is refreshed to upstream's `ptrload`; `tests/sdk/ilp32-tls-models-codegen-test.sh` and the `tls-ie` row stay as the TLS guards |
| 0033 | retired (0059 canonicalises a pointer taken from a by-value aggregate packed in one GPR); guarded by `tests/sdk/ilp32-packed-arg-codegen-test.sh` |

Exit: full release gate; every ILP32 regression row and codegen test passes
(`tests/regression/ilp32-*`, `cxx-eh`, `tests/sdk/ilp32-*-codegen-test.sh`,
`minimal-toc-ilp32-test.sh`); RPCS3 boots on the PPU interpreter, which, unlike
the recompiler, faults on a set bit 32; the list of retired patches.

## Stage 1: C sources ready for GCC 14 and 15 defaults

Independent of the compiler and can run beside stage 0.  GCC 14 made
`-Wimplicit-function-declaration`, `-Wimplicit-int`, `-Wint-conversion` and
`-Wincompatible-pointer-types` errors.  GCC 15 made C23 the default C
standard, where `bool`, `true` and `false` are keywords and `f()` means
`f(void)`.

Build the newlib glue, PSL1GHT, the SDK libraries, portlibs and every sample
on 12.4 with those warnings as errors and with `-std=gnu2x`, and fix what
fails.  This lands as ordinary source fixes in normal releases.

This is preparation only.  GCC 12.4's `-std=gnu2x` predates the C23 changes
that matter here: `bool`, `true` and `false` became keywords and unprototyped
`f()` came to mean `f(void)` in GCC 13, and GCC 15 made `gnu23` the default
(12.4 does not accept `-std=gnu23`).  So `typedef int bool;` and calling an
`int f();` declaration with arguments still pass this check.  Actual C23
compatibility is validated in stage 2, with the target compiler.

Exit: the full SDK and sample build is clean under the flags above on 12.4.

## Stage 2: PPU on GCC 16

**Status: the switch is made in the build and release scripts, effective in
the release the director approves for it.**  `build-ppu-toolchain.sh` builds
GCC 16.1.0 only; its `--gcc` selector and the GCC 12.4 path are gone from the
build and release scripts, and `bootstrap.sh` fetches `releases/gcc-16.1.0`.
`patches/ppu/gcc-12.x` stays in the tree as the port map's reference.  Until
that release is tagged, the last published release still ships GCC 12.4.0.
The exit criteria below that have not been measured remain pending; the stage
is done only when they pass.  The ABI-compatibility check is superseded by C1:
objects and archives built by a 12.4 release are not compatible and must be
rebuilt.

A new patch directory `patches/ppu/gcc-16.x/`, ported patch by patch from the
post-stage-0 series and tracked in a port map: each 12.4 patch is marked
ported, rewritten, retired (with the upstream change that made it
unnecessary) or dropped (with the reason).

| Group | 12.4 patches | Port risk |
|---|---|---|
| Target recognition, specs, cell64lv2 headers, multilibs | about 20 (most new files) | low.  `dbxelf.h` is gone since GCC 13 and leaves the tm_file lists |
| libgcc, libstdc++ crossconfig | a few | low to medium |
| Pointer model (0005, 0059 and its follow-ups 0060 to 0065, the survivors of stage 0) | as few as stage 0 leaves | high: rs6000 address legitimisation has been reworked several times since 12 |
| Remaining middle-end changes (`convert.cc`, `expr.cc`) | whatever stage 0 cannot retire | high: shared code, so every one is a candidate for an upstream submission or a target hook instead |
| Codegen fixes (thunks, TLS models, TOC groups, tablejump) | about 6 | medium: each has its own regression test |

The new compiler is built beside 12.4 into its own prefix, never over it.

Checks beyond the release gate:
- **A/B codegen** against 12.4 on the whole SDK (the candidate-versus-control
  method: the same configure line, `make all-gcc`, compared with `-B`).  Report
  static size and loop-instruction deltas per archive, and explain any change
  over 2%, per the performance rule for GCC changes.
- **ABI compatibility (superseded by C1).**  The original check: objects and
  archives built by 12.4 link with objects built by 16, in both data models,
  and run (C++ exceptions thrown across the boundary, TOC calls, TLS, varargs,
  struct passing), with matching data-model tags (attribute 16), so that an
  existing v0.21.x archive needs no rebuild.  C1 changed the `char`,
  `wchar_t`, `wint_t` and `errno` ABI, so mixing is not supported: every
  object and archive is rebuilt with the new release's toolchain (GCC 16 for
  PPU, the release's GCC 9.5 for SPU), C and C++.
- **Carry patterns.**  `count += (x != 0xff102030u)` once counted wrong in
  loops in both data models, because our patches let 32-bit carry patterns
  run on 64-bit registers (fixed in v0.16.4).  No carry pattern is widened in
  the 16 port, and the `ppu-compare-carry` rows pass in both data models at
  -O1 to -O3.
- **C23 default.**  The C sources of the SDK libraries, newlib glue, PSL1GHT,
  portlibs and every sample build with the 16-based compiler at its default
  `-std=gnu23`, and every C-compatible public PPU header compiles alone in a
  `gnu23` C translation unit.  This is the real check of what stage 1
  prepared.  C++-only headers and samples keep their C++17 checks, and the SPU
  headers and sources are checked in stage 3, with the SPU compiler.
- **DWARF.**  GCC 11 and later default to DWARF 5.  GDB 14 reads it; RPCS3 and
  other consumers need checking, or the specs pin `-gdwarf-4`.

Rollout: the first release with the new compiler ships it as the default, and
the previous release stays the documented fallback.  No release ships both.

Exit: full release gate on the 16-based package, plus the five checks above,
plus EMP acceptance.

Port map (`patches/ppu/gcc-16.x/`, against `releases/gcc-16.1.0`; every
patch applies with `--fuzz=0`):

| Patch | GCC 16 result |
|---|---|
| the other 38 | ported unchanged: byte-identical to `gcc-12.x` |
| 0005, 0048, 0061, 0067 | ported, context refreshed: their hunks needed fuzz against 16.1; the added and removed lines are unchanged |
| 0001, 0024 | rewritten: `dbxelf.h` (removed in GCC 13 with STABS) and `usegas.h` (removed in 16) leave the `tm_file` lists.  `usegas.h` only defined `USE_GAS`, which is read by the Solaris and HP-UX headers, none of them ours; context refreshed |
| 0009, 0010 | rewritten: each also adds the `.opt.urls` file that GCC 14 and later require beside every `.opt`.  Our options are not in the GCC manual, so the file holds only the header line that `regenerate-opt-urls.py` would emit |

The series reproduces the tree that built the exploratory compiler and passed
the stage-2 guards byte for byte (`tests/host/gcc16-series-test.sh`).

## Stage 3: SPU on GCC 16

GCC 10 removed the SPU backend: about 6,800 lines in `gcc/config/spu`, 3,200
in `libgcc/config/spu`, 2,500 of intrinsic headers and 21,000 lines of tests.
`patches/spu/FORWARD_PORT_README.md` sketched a port to GCC 12 at 12 to 24
weeks; to 16 it is the same work against newer APIs.  It lives on its own
branch, builds into its own prefix, and the 9.5 SPU compiler stays the shipped
one until it passes.

Work, largest first:
1. **Register allocation.**  The SPU port most likely used the old reload
   pass (to confirm on the restored source).  Current GCC expects LRA, so the
   backend would need `TARGET_LRA_P`, with its constraints and secondary
   reloads reworked.  This is the main risk and the main
   unknown, so it goes first, as a spike: build `libgcc` and newlib with the
   restored backend under LRA before committing to the rest.
2. **Restore and rename** `gcc/config/spu/*` (.c to .cc), the libgcc sources,
   the `config.gcc` and `config.host` entries, and the intrinsic headers.
3. **Target hooks** whose signatures changed since 9 (scalar_int_mode and
   fixed_size_mode variants, `TARGET_LEGITIMATE_ADDRESS_P` with its new
   argument, the costs hooks).  Each is checked against the 16 `tm.texi`.
4. **Our SPU patches.**  The 14 9.5 patches port onto the restored backend.
   0014 (parenthesised vector literals in the C and C++ parsers) is a rewrite:
   both parsers have changed throughout since 9.
5. **Tests.**  The restored `gcc.target/spu` testsuite, run on the SPU
   simulator if one builds, otherwise compile-only with assembly checks.

Exit: full release gate, plus the SPURS suite, every SPU sample and SPU job in
RPCS3, codegen A/B against 9.5 on the SPU libraries, and EMP acceptance (EMP's
engine runs SPU jobs).  At this point PPU and SPU share one GCC.

## What does not move

binutils 2.42 (spu-elf is still upstream), newlib 4.4.0 and GDB 14.2 stay as
they are.  Each can move later, alone, under the same rule.

## Order and estimates

| Stage | Depends on | Estimate |
|---|---|---|
| 0 pointer model on 12.4 | | 1 to 2 weeks |
| 1 C sources | | about 1 week, beside stage 0 |
| 2 PPU on 16 | 0, 1 | 3 to 6 weeks |
| 3 SPU on 16 | 1; the LRA spike can start any time | 12 to 24 weeks |

## Decisions (approved 2026-10-14)

1. GCC 16 as the target line.
2. The stage order, with the pointer model on 12.4 first.
3. One compiler per release: no package ships 12.4 and 16 side by side.
