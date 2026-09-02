#ifndef RSX_CG_COMPILER_COMPILE_OPTIONS_H
#define RSX_CG_COMPILER_COMPILE_OPTIONS_H

/*
 * rsx-cg-compiler — compile-time option surface.
 *
 * Models the subset of command-line flags that actually change output
 * bytes, plus room for ones that will matter once we lower real-world
 * shaders.  Our defaults MUST match the defaults on the other side of
 * the byte-exact diff harness, so that it compares like-for-like without
 * requiring extra flags on either side of the test.
 *
 * Today's reality check:
 *
 *   - At least TWO parts of the surface change output on the shaders we
 *     emit.  (An earlier note here said "exactly ONE"; that was measured
 *     before the optimization levels were probed with the right flag
 *     spelling, and it is false — see the O0..O3 entry below.)
 *       * `--fastmath` (default) vs `--nofastmath` — fragment-only;
 *         the latter disables MAD fusion, keeps temps, promotes
 *         COLOR varyings to fp16 (H-regs).
 *       * the `O0..O3` levels.
 *   - `--O0..O3` DO change output.  An earlier note here recorded them
 *     as no-ops; that was measured only on our curated shaders, which
 *     contain almost nothing to optimize, and it predicted its own
 *     expiry in the same breath ("they'll start to diverge once shaders
 *     exercise dead code, redundant arithmetic, unrollable loops").
 *     Re-measured 2026-08-31 over a 116-shader community fragment
 *     corpus: 67 shaders produce DIFFERENT BYTES between the lowest and
 *     highest level, 49 are identical, 1 fails.  And on OUR OWN 18
 *     in-repo shaders - the very corpus the "no-ops" claim was made
 *     against - 4 differ across levels.  So the original claim was not
 *     merely narrow, it was wrong about the shaders it was measured on.
 *     Treat the levels as load-bearing.
 *
 *     A trap worth keeping written down, because it silently produces a
 *     "levels do nothing" result: on the other side of the harness the
 *     levels are spelled with a DOUBLE dash.  A single-dash spelling is
 *     ignored
 *     rather than rejected, so the sweep compares two identical default
 *     compiles and concludes the flag has no effect.
 *   - VP output is completely insensitive to every optimisation flag
 *     probed — the VP pipeline is effectively hand-scheduled.
 *
 *   - OUR OWN output does not vary with the level AT ALL: all 18 in-repo
 *     shaders emit byte-identical containers at O0, O1, O2 and O3,
 *     because nothing in our pipeline gates on `opt` yet.  Two things
 *     follow.  First, changing our default level cannot change our
 *     bytes, so it moves no comparison number - it makes the DECLARED
 *     default honest, nothing more.  Second, the real work is still
 *     ahead: passes have to actually gate on this field before the
 *     level means anything on our side.
 *
 * Design contract: every *real optimization pass* we write should
 * gate on `CompileOptions` at the IR / lowering layer.  When a pass
 * matters for correctness (e.g. the back-face colour bit on the VP
 * output mask, where the harness's other side emits front-face-only
 * by default), it goes on the direct toggle (`backFaceColorBits`), not the coarse
 * opt level.  When a pass is purely "go faster" (e.g. MAD fusion),
 * it gates on `opt` and/or `fastmath`.
 */

namespace rsx_cg
{

enum class OptLevel
{
    // `--O0` — disable most optimizations: keep literal expression
    // trees, no register reuse, no dead-code removal.  (An earlier
    // comment here called the FP levels unimplemented in the shaders we
    // had probed.  That is false: 4 of our own 18 in-repo shaders and 67
    // of 116 community shaders change bytes across the levels.)
    O0 = 0,

    // `--O1` — light pass: dead-code elimination, constant folding,
    // remove unused parameters.  OUR DEFAULT, and the level the diff
    // harness pins against.
    O1 = 1,

    // `--O2` — MAD fusion, register reuse across disjoint lifetimes,
    // common-subexpression elimination.
    O2 = 2,

    // `--O3` — aggressive: also cross-basic-block propagation, more
    // speculative reg reuse.  Not distinguishable from O2 in our
    // probes today.
    O3 = 3,
};

struct CompileOptions
{
    // ------ Optimisation level ------
    // Default level.  Changed from O2 on 2026-08-31 for DECLARATION
    // PARITY with the other side of the diff harness, whose no-flag
    // default reproduces O1.
    //
    // Be precise about what this did and did not fix.  It did NOT
    // correct any existing byte-identity number: our own output is
    // identical at all four levels today (all 18 in-repo shaders),
    // because nothing here gates on `opt` yet, so our O2 bytes and our
    // O1 bytes are the same bytes.  The old mismatch was wrong in the
    // LABEL and empty in the OUTPUT.  What it buys is that the first
    // pass to actually gate on this field will be compared like-for-like
    // instead of silently across levels.
    OptLevel opt      = OptLevel::O1;
    bool     fastmath = true;           // default ON (MAD fusion, fp16 COLOR promote)

    // ------ Code-generation knobs that affect output bytes directly ------
    // (--bcolor is documented as default ON, but the observed output sets
    // ONLY the front-face bit in attributeOutputMask — our VpAssembler
    // matches the observed behaviour, not the documented one.  If we ever need back-face bits, flip
    // this and teach VpAssembler to emit them.)
    bool backFaceColorBits = false;

    // Keep unused parameters in the parameter table (`--fkeep-unused`).
    // Stripped by default on both sides of the harness.
    bool keepUnusedParams = false;

    // Enable NRMH instruction in FP supporting profiles (`--fuse-nrmh`).
    // Default ON.
    bool useNrmh = true;

    // Early scheduling of kills + alphakill-enabled fetches
    // (`--fearly-kills`).  Default ON.
    bool earlyKills = true;

    // HW bug workaround: clamp point size > 0.125 (`--fmax-psize`).
    // Default ON.
    bool maxPsizeWorkaround = true;

    // The NV40 general lowering pipeline: flatten, lower, legalise,
    // schedule, allocate.  Default ON since D1 - it is the compiler now.
    // The shape matcher it replaced is still reachable, for one release,
    // as `--legacy-lowering`; a shader that only the matcher compiles is
    // a bug against this path, and the flag is there to prove it rather
    // than to live with it.
    bool generalLowering = true;

    // ------ Placeholders for features we'll plumb as they're needed ------
    // --disablepc <all|attrno>   : disable perspective-correct interp
    // --texsign ...              : signed texture remapping
    // --texformat ...             : texture-format hints for partialTexType
    // --inline <all|none|count=N>: inlining policy
    // --unroll <all|none|count=N>: unroll policy
    // --ifcvt <default|all|...>  : if-conversion policy
};

// Predicate helpers — let per-pass gating read in a legible style.
constexpr bool atLeast(OptLevel have, OptLevel want)
{
    return static_cast<int>(have) >= static_cast<int>(want);
}

inline CompileOptions defaultOptions()
{
    return CompileOptions{};
}

}  // namespace rsx_cg

#endif  /* RSX_CG_COMPILER_COMPILE_OPTIONS_H */
