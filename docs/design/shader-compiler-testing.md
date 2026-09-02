# Shader compiler testing — corpus, baselines, pass criteria

Design for the shader-compiler milestone: collect community Cg shaders as
test inputs, compile them with `tools/rsx-cg-compiler` (Cg → NV40,
`sce_fp_rsx`/`sce_vp_rsx` profiles), gate regressions on the results, and
judge what can be judged at runtime on RPCS3.

## 1. Corpus policy

Test inputs fall into exactly three license classes.  The class decides
where a shader may live, and the fetch tooling enforces it.

| class | meaning | where it lives |
|---|---|---|
| `vendor` | ours, or license-compatible (MIT/BSD/zlib) | committed, under `tools/rsx-cg-compiler/tests/` or `samples/` |
| `fetch-run-only` | usable as *input* to a tool, not as *content* of this repo (GPL and friends) | fetched at a **pinned commit** into a **gitignored** corpus dir by `scripts/fetch-shader-corpus.sh`; never committed |
| `excluded` | license unknown or prohibitive | listed in the license table with the reason; not fetched |

Running a compiler over a GPL shader does not attach the GPL to the
compiler or to this repository; committing the shader would.  The fetch
script therefore writes only into `build/`-side corpus directories, and
the corpus lane's acceptance includes `git status` proving zero fetched
files are tracked.

Two rules that follow:

- **Ambiguous license → `fetch-run-only`, never `vendor`.**  Running is
  not redistribution, so fetch-run-only is safe under almost any
  license; *committing* is the act that needs certainty.  A discrepancy
  between a repo's license file and its registry metadata (e.g. a
  BSD-looking file under a GitHub NOASSERTION) is ambiguity — the file
  stays fetch-run-only until the discrepancy is resolved with the
  upstream, not adjudicated by whoever reads it first.
- **Vendored third-party shaders carry their provenance.**  Permissive
  (MIT/BSD) sources that pass verification are committed into a
  third-party corpus directory — one subdirectory per source with that
  source's copyright and license text alongside the files — never
  flat-dumped into our own MIT tests directory.  What permissive
  vendoring buys is real: a committed shader runs in the regression
  battery on any machine with no fetch script, no pinned-SHA drift, and
  no network in CI, which is why the ~16 certain-permissive shaders
  (plus 5 contingent, below) are worth more per shader to the suite
  than the ~106 GPL ones.

Initial source list (the corpus lane owns extending it, with a license
table row per source):

- **Ours**: 18 `.cg` shaders in `tools/rsx-cg-compiler/tests/shaders/`
  and 41 tracked `.vcg`/`.fcg` in `samples/` — `vendor` class, no fetch
  needed.  (Counts are of *tracked* files: two stray copies in a
  `buildr/` output dir are build products, not corpus.  Note the three
  extensions — a corpus glob must match `.cg`, `.vcg` AND `.fcg`.)

  **Repo-hygiene prerequisite:** `.gitignore` currently blanket-ignores
  `/tools/rsx-cg-compiler/tests/` to keep local-only material out of
  the tree, even though the 18 shader sources inside are tracked.
  Any *new* corpus shader added there today is silently invisible to
  `git add` and to ignore-honouring tools (this produced a false "no
  test corpus exists" reading during enumeration).  Before the MIT
  corpus lands: narrow the rule to a `tests/local/` subdirectory,
  relocate any local-only pieces into it on machines that carry
  them, and un-ignore the rest of `tests/`.
- **PS3 OpenGraphics Toolkit** `testsuite/shaders/` — 86 fragment + 1
  vertex Cg shaders, GPL-2.0 → `fetch-run-only`.  Their 86 feature names
  are facts and are mirrored by our own MIT corpus (one shader per
  feature, written by us; the toolkit's tests 69–86 are stubs even in the
  toolkit, so ours is the first implementation of those).
- **IoQuake3-PS3** (Mayo1970, GPL-2.0) — 8 Cg shaders at the pinned
  commit → `fetch-run-only`.
- **xash3d-fwgs** (Mayo1970) — measured: **zero** `.cg/.fcg/.vcg` files;
  it is a *validation port target* (sequencing step 6), not a corpus
  source.  Listed here so nobody re-adds it to the corpus table.
- Further ecosystem sources (measured at pinned commits during
  enumeration): **crystalct/PSL1GHT** develop — 6 distinct shader blobs,
  MIT → `vendor`-eligible after per-file license verification;
  **RSXGL** — 10 Cg, BSD-style → `vendor`-eligible likewise;
  **th06_ps3** — 11 Cg, GPL-3.0 → `fetch-run-only`;
  **ClassiCube** — 5 Cg, license file BSD-3-Clause-style but GitHub
  reports NOASSERTION → **ambiguous, therefore `fetch-run-only`** per
  the rule above; vendoring these five requires the **director's
  determination** on the discrepancy (or upstream resolving it) — a
  legal judgement about redistributing someone else's code is not an
  engineering call.
  The license table (corpus lane) is authoritative; `vendor`-eligible
  means *may* be committed once the per-file check passes, not that it
  has been.

## 2. Compilers and baselines

| tool | role | output handling |
|---|---|---|
| `rsx-cg-compiler` | the compiler under test | normal build artifacts |
| `cgcomp` (vendored) | community baseline — **conditional**: requires NVIDIA's `Cg` runtime (`cg.dll`), which we deliberately do not stage, so it runs only on hosts that provide it | comparable where it runs |
| PS3 OpenGraphics Toolkit binaries | community baseline, run as a black box | local-only (GPL: run, never copy) |
| RPCS3 (desktop release build) | runtime baseline | logs preserved per the boot rules |

## 3. Pass criteria — three tiers

Every corpus shader gets a row at each tier it is eligible for.

- **Tier a — compile + container.**  Our compiler accepts the shader (or
  rejects it with a diagnostic that names the unsupported feature — a
  silent wrong-code compile is the failure mode this milestone exists to
  kill).  The emitted container passes the existing structural checks.
  Rejection must come from a *diagnostic*, never from resource
  exhaustion: memory use scales with shader size; a large source may
  take longer, not fail.
- **Tier b — microcode round-trip.**  Our disassembler renders the
  emitted NV40 microcode; the render re-assembles/compares clean.
  **Aspirational today:** the compiler has no disassembler yet
  (`--emit-container`, `--dump-ast`, `--dump-ir` exist; nothing renders
  microcode back to text).  Tier b lands when that tool does; until
  then the public suite has no microcode-level verification.
- **Tier c — runtime readback (the judged tier).**  A harness renders
  each eligible test to an off-screen render target on RPCS3, reads the
  pixels back on the PPU, compares against expected values computed on
  the PPU for the same inputs, prints `PASS/FAIL <name>` to TTY and exits
  non-zero on any failure.  This is what the toolkit's visual-only
  harness lacks, and it slots into `tests/regression/` as new rows.
  Visual spot-checks on RPCS3 remain for the demos that have no closed
  form (e.g. the ray-trace test), and the director's GUI checks.

## 4. Harness architecture

- `scripts/fetch-shader-corpus.sh` — pinned-commit fetches into
  gitignored dirs, one function per source, license class asserted.
- `scripts/shader-compile-check.py` — the compile gate: drives our
  compiler over a corpus directory (tracked corpus by default; fetched
  corpora via an explicit external-corpus mode) and emits a fixed-column
  `results.csv` keyed `(shader, compiler)`: source, license class,
  tier-a result, diagnostics, tier-b result.  It refuses an empty corpus
  and supports a minimum-count assertion — a gate over zero shaders
  passes trivially, which is the one way it could report success while
  testing nothing.
- Tier c ships as `tests/regression/` rows once the readback harness
  exists.

Deeper output-comparison methodology lives in internal engineering
notes, not in this document.

## 5. Known gaps the corpus must cover

From the feature list versus our 18-test corpus, the expected holes:
derivatives (`ddx`/`ddy`), `discard`, dynamic branching and loops, MRT
(`COLOR1..3`), `DEPTH` output, `tex2Dlod`/texRECT/derivative fetches,
3D/cube sampling, centroid interpolation, `VFACE`, pack/unpack ops,
`LIT`, bump-env (`BEM`), projective texture (`TXP`), halfword/bitwise
ops, address-register stack, predicates, point size, fog.  Each gets a
`vendor`-class corpus shader whether or not the compiler supports it yet
— unsupported features must fail tier a *loudly* today and flip to green
as they land.

Measured against real community shaders (2026-08-31): the shape matcher
compiled a narrow subset of them, while the general lowering path
compiled nearly all.  That gap is why the general path is the default
since 2026-09-02; the matcher is `--legacy-lowering` for one release, so
a divergence can be bisected against it rather than argued about.  Tier c
readback remains the instrument that decides whether the general path's
output is correct and not merely different — the flip did not settle
that question, it moved which answer ships.

## 6. Sequencing

1. This document reviewed by both leads.
2. Corpus enumeration + license table; fetch script.
3. Compile gate over the in-repo 59 shaders (18 + 41 tracked), then the
   fetched corpora.
4. Our MIT corpus for the 86 feature names, prioritized by the gap list.
5. Readback harness → regression rows.
6. The ports (IoQuake3-PS3, xash3d-fwgs) built against our compiler as
   the real-world exit test.
