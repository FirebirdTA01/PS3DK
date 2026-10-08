# One modern GCC for PPU and SPU

Status: plan, for approval.  Today the PPU compiler is GCC 12.4.0 and the SPU
compiler is GCC 9.5.0, the last release with the SPE backend.  This plan moves
both to one current GCC release line without changing the ABI, and ships the new
compiler only after it passes every release gate.

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

Under ILP32, `Pmode` is SImode today (patch 0005), so GCC treats a pointer's
upper 32 bits as don't-care, while the PPU forms effective addresses from all
64 bits.  Twenty-three patches work around the consequences one bug at a time:
the `Pptr` iterator rewrites of `rs6000.md` (0012, 0015, about 900 diff lines),
middle-end canonicalisation in `convert.cc` and `expr.cc` (0029, 0030, 0032,
0033, 0055), DImode saves of callee-saved, LR and EH registers (0031, 0037),
the `-fno-ivopts` default (0049) and the doloop counter fix (0050).

Patch 0051 (on `feat/ilp32-pmode-di`, card t_3b6c018a) changes the model
instead: `Pmode` DImode, `ptr_mode` SImode, `POINTERS_EXTEND_UNSIGNED`.
Pointers are zero-extended where they become addresses, so a set bit 32 can
no longer reach the address unit, whichever path produced it.

Stage 0 lands 0051 to 0054 on 12.4, then retires every workaround the new model
subsumes, one patch at a time, each removal proven by its regression row.
Doing this before the version change matters for two reasons: the port carries
the small model instead of the workaround stack, and the model change is
validated on the compiler whose behaviour we already know.

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
fails.  The SDK headers must also compile under `-std=gnu23` in user code.
This lands as ordinary source fixes in normal releases.

Exit: the full SDK and sample build is clean under the flags above.

## Stage 2: PPU on GCC 16

A new patch directory `patches/ppu/gcc-16.x/`, ported patch by patch from the
post-stage-0 series and tracked in a port map: each 12.4 patch is marked
ported, rewritten, retired (with the upstream change that made it
unnecessary) or dropped (with the reason).

| Group | 12.4 patches | Port risk |
|---|---|---|
| Target recognition, specs, cell64lv2 headers, multilibs | about 20 (most new files) | low.  `dbxelf.h` is gone since GCC 13 and leaves the tm_file lists |
| libgcc, libstdc++ crossconfig | a few | low to medium |
| Pointer model (0005, 0051, the survivors of stage 0) | as few as stage 0 leaves | high: rs6000 address legitimisation has been reworked several times since 12 |
| Remaining middle-end changes (`convert.cc`, `expr.cc`) | whatever stage 0 cannot retire | high: shared code, so every one is a candidate for an upstream submission or a target hook instead |
| Codegen fixes (thunks, TLS models, TOC groups, tablejump) | about 6 | medium: each has its own regression test |

The new compiler is built beside 12.4 into its own prefix, never over it.

Checks beyond the release gate:
- **A/B codegen** against 12.4 on the whole SDK (the candidate-versus-control
  method: the same configure line, `make all-gcc`, compared with `-B`).  Report
  static size and loop-instruction deltas per archive, and explain any change
  over 2%, per the performance rule for GCC changes.
- **ABI compatibility.**  Objects and archives built by 12.4 link with objects
  built by 16, in both data models, and run: C++ exceptions thrown across the
  boundary, TOC calls, TLS, varargs, struct passing.  The data-model tag
  (attribute 16) must agree.  An existing v0.21.x archive must not need a
  rebuild.
- **The known 12.4 miscompile.**  `x != 0xff102030u` counted wrong in loops
  (both data models, -O1 and above).  Run it on 16: fixed upstream, or ours to
  find.
- **DWARF.**  GCC 11 and later default to DWARF 5.  GDB 14 reads it; RPCS3 and
  other consumers need checking, or the specs pin `-gdwarf-4`.

Rollout: the first release with the new compiler ships it as the default, and
the previous release stays the documented fallback.  No release ships both.

Exit: full release gate on the 16-based package, plus the three checks above,
plus EMP acceptance.

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

## Decisions asked for

1. GCC 16 as the target line.
2. The stage order, with the pointer model on 12.4 first.
3. One compiler per release: no package ships 12.4 and 16 side by side.
