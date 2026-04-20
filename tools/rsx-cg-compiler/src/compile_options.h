#ifndef RSX_CG_COMPILER_COMPILE_OPTIONS_H
#define RSX_CG_COMPILER_COMPILE_OPTIONS_H

/*
 * rsx-cg-compiler — compile-time option surface.
 *
 * Models the subset of `sce-cgc` command-line flags that actually
 * change output bytes, plus room for ones that will matter once we
 * lower real-world shaders.  The default values MUST match sce-cgc's
 * defaults so our byte-exact diff harness compares like-for-like
 * without requiring extra flags on either side of the test.
 *
 * Today's reality check (probed against a 3.55-4.75-era cell SDK):
 *
 *   - Of the ~60 sce-cgc CLI flags, exactly ONE actually changes
 *     output on the shaders we emit:
 *       * `--fastmath` (default) vs `--nofastmath` — fragment-only;
 *         the latter disables MAD fusion, keeps temps, promotes
 *         COLOR varyings to fp16 (H-regs).
 *   - `--O0..O3` are currently no-ops in the shaders we probed
 *     (help text says "only affects sce_fp_rsx" but all four levels
 *     produced byte-identical output for both simple and complex FP
 *     shaders).  They'll start to diverge once shaders exercise
 *     dead code, redundant arithmetic, unrollable loops, inlinable
 *     functions.  Architecture below preserves the full O0..O3 axis
 *     so future gates can land cleanly.
 *   - VP output is completely insensitive to every optimisation flag
 *     probed — the VP pipeline is effectively hand-scheduled.
 *
 * Design contract: every *real optimization pass* we write should
 * gate on `CompileOptions` at the IR / lowering layer.  When a pass
 * matters for correctness (e.g. the back-face colour bit on the VP
 * output mask, which sce-cgc emits front-face-only by default), it
 * goes on the direct toggle (`backFaceColorBits`), not the coarse
 * opt level.  When a pass is purely "go faster" (e.g. MAD fusion),
 * it gates on `opt` and/or `fastmath`.
 */

namespace rsx_cg
{

enum class OptLevel
{
    // `--O0` — disable most optimizations.  Expected behaviour (not
    // yet implemented by sce-cgc for FP in the shaders we've probed):
    // keep literal expression trees, no register reuse, no dead-code
    // removal.
    O0 = 0,

    // `--O1` — light pass: dead-code elimination, constant folding,
    // remove unused parameters.  Currently indistinguishable from
    // default in our probes.
    O1 = 1,

    // `--O2` — sce-cgc default.  MAD fusion, register reuse across
    // disjoint lifetimes, common-subexpression elimination.  This is
    // the level our diff harness pins against.
    O2 = 2,

    // `--O3` — aggressive: also cross-basic-block propagation, more
    // speculative reg reuse.  Not distinguishable from O2 in our
    // probes today.
    O3 = 3,
};

struct CompileOptions
{
    // ------ Optimisation level ------
    OptLevel opt      = OptLevel::O2;   // sce-cgc default
    bool     fastmath = true;           // default ON (MAD fusion, fp16 COLOR promote)

    // ------ Code-generation knobs that affect output bytes directly ------
    // (sce-cgc docs say --bcolor is default ON but sce-cgc-output sets
    // ONLY the front-face bit in attributeOutputMask — our VpAssembler
    // matches that behaviour.  If we ever need back-face bits, flip
    // this and teach VpAssembler to emit them.)
    bool backFaceColorBits = false;

    // Keep unused parameters in the parameter table (`--fkeep-unused`).
    // sce-cgc strips them by default.
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
    return CompileOptions{};  // all fields already sce-cgc-default
}

}  // namespace rsx_cg

#endif  /* RSX_CG_COMPILER_COMPILE_OPTIONS_H */
