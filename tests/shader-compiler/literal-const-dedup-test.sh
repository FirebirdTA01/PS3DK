#!/usr/bin/env bash
# A literal vec4 store packs the DISTINCT values and selects with the source
# swizzle, the way the reference compiler does (t_642eb36e).
#
# THE RULE, derived from the oracle over the thirteen shapes below and
# independently already present in the legacy emitter (FpConstBlockPacker,
# nv40_fp_emit.cpp): the block holds the distinct values in FIRST-APPEARANCE
# order, zero-filled, and swizzle[lane] is the index of that lane's value.
# Equality is `==`, NOT the bit pattern, and the representative is whichever
# value appeared first - measured BOTH ways round, which is the only way to
# see that there is no normalisation:
#
#   float4(0.0, -0.0, 0.0, -0.0)   packs +0   read .xxxx
#   float4(-0.0, 0.0, -0.0, 0.0)   packs -0   read .xxxx
#
# THIS SCRIPT USED TO RUN THE LEGACY PATH.  The property is reference parity,
# and it was asserted only under --legacy-lowering while the SHIPPING path
# silently failed it - t_8a7aa04d's finding, one of nine.  The subject is now
# the shipping path; ONE legacy invocation survives as a differential control.
#
# TOMBSTONE FOR THAT CONTROL: the legacy matcher is retired.  When it goes,
# delete the differential row - do not "fix" it, and do not let it become the
# subject again.  Its only job today is to show that two independently
# written implementations of one rule agree.
#
# THE ARTIFACT IS THE CONTAINER, and it is the ONLY artifact: the packing,
# the swizzle and the predication below are all read from the same bytes.
# This used to read the stdout LISTING - and the listing was NOT missing the
# condition fields: it dumps all four raw words per row and this file already
# unswapped them (review: codex).  The gap was that nothing here LOOKED at
# the condition test (hw[1] bits 18..20) or the condition-code write (hw[0]
# bit 8), so a container whose colour MOV carried NVFX_COND_FL - a write that
# never executes - would carry a perfectly packed block that never reached a
# pixel and every row below would still pass.  (t_7396e0c2's rows had the
# harder version of it: they read fp_sources' RENDERED text, which really
# does omit those fields.)
#
# A word-level check over the listing would have closed it too.  The
# container was chosen because it puts the packing, the swizzle and the
# predicate on ONE set of bytes: the sixteen expectations below were read off
# reference CONTAINERS anyway, and a second decoder over a second artifact is
# a second mutation surface.  predication_check.py reads the fields, and all1
# flips its own colour write to COND_FL in the container bytes and requires
# that same predicate to reject it BY REASON.
#
# BOTH HALVES OF THE PACKING ARE ASSERTED ON PURPOSE.  A packed block read
# with an identity swizzle paints the wrong colour - float4(1,0.5,1,0.5)
# would come out (1,0.5,0,0) - so a test that checked the block alone would
# pass on a miscompile.  The three controls at the end feed the comparison a
# right block with a wrong swizzle, a right swizzle over a wrongly ordered
# block, and an un-deduped block, and require each to be rejected by name.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"
PYTHON="${PYTHON:-python3}"
command -v "$PYTHON" >/dev/null 2>&1 || PYTHON=python
predication_checker="$here/predication_check.py"
[[ -f "$predication_checker" ]] || fail "predication_check.py is missing"

work="${TMPDIR:-/tmp}/ps3dk-literal-const-dedup-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

# compile <source> <tag> [extra flags...] - the artifact is $work/<tag>.fpo.
# stdout is kept only so a compile failure can print its own diagnostic; no
# assertion in this file reads it.
compile() {
    local src="$1" tag="$2"; shift 2
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$work/$tag.fpo" "$@" "$src"
    ) >"$work/$tag.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$tag timed out"
    if [[ "$rc" -ne 0 ]]; then
        tail -n 20 "$work/$tag.log" >&2
        fail "$tag failed to compile"
    fi
    [[ -s "$work/$tag.fpo" ]] || fail "$tag.fpo is missing or empty"
}

# THE SHAPES.  Every expected block and swizzle below was read off the
# REFERENCE compiler's own container, not off ours: tag|expression|four
# block words|swizzle.  The last two are the -0 pair and they are compared
# as WORDS, because 0.0 == -0.0 and a float comparison cannot see them.
SHAPES=(
    "all1|float4(1.0, 1.0, 1.0, 1.0)|3f800000 00000000 00000000 00000000|xxxx"
    "bcast|float4(0.375)|3ec00000 00000000 00000000 00000000|xxxx"
    "half_pair|float4(1.0, 0.5, 1.0, 0.5)|3f800000 3f000000 00000000 00000000|xyxy"
    "quarters|float4(0.5, 0.25, 0.5, 0.125)|3f000000 3e800000 3e000000 00000000|xyxz"
    "all_distinct|float4(0.5, 0.25, 0.125, 1.0)|3f000000 3e800000 3e000000 3f800000|xyzw"
    "zeros|float4(0.0, 0.0, 0.0, 0.0)|00000000 00000000 00000000 00000000|xxxx"
    "zero_one|float4(0.0, 1.0, 0.0, 1.0)|00000000 3f800000 00000000 00000000|xyxy"
    "adjacent_pairs|float4(2.0, 2.0, 3.0, 3.0)|40000000 40400000 00000000 00000000|xxyy"
    "signed_pair|float4(-1.0, 1.0, -1.0, 1.0)|bf800000 3f800000 00000000 00000000|xyxy"
    "tail_run|float4(7.0, 1.0, 1.0, 1.0)|40e00000 3f800000 00000000 00000000|xyyy"
    "three_values|float4(1.0, 2.0, 1.0, 3.0)|3f800000 40000000 40400000 00000000|xyxz"
    "negzero_after|float4(0.0, -0.0, 0.0, -0.0)|00000000 00000000 00000000 00000000|xxxx"
    "negzero_first|float4(-0.0, 0.0, -0.0, 0.0)|80000000 00000000 00000000 00000000|xxxx"
)

for shape in "${SHAPES[@]}"; do
    IFS='|' read -r tag expr _blk _swz <<<"$shape"
    printf 'float4 main(float4 p : TEXCOORD0) : COLOR\n{\n    return %s;\n}\n' \
        "$expr" > "$work/$tag.cg"
    compile "$work/$tag.cg" "$tag"
done

# The NON-FINITE row.  A repeated infinity merges like any equal pair, and
# the merged form PAINTS THE SOURCE: block {inf,1,2,0} read .xyxz gives
# (inf,1,inf,2) back.  There is no special case for it and there must not
# be one - an earlier draft had an isfinite() guard and it protected
# nothing (review: codex).
#
# NAMED DIVERGENCE, and the wording is deliberate: THE REFERENCE'S PACKING
# OF REPEATED NON-FINITE VALUES IS NOT CHARACTERISED.  Four arrangements
# gave four different results, and every one paints values the source does
# not contain (t_b737691f):
#
#   float4(inf,1,inf,2)   ref {inf,1,2,0} .xyzz  paints (inf,1,2,2)
#   float4(inf,1,3,inf)   ref {3.0e38,0,0,0} .xxxx paints 3.0e38 x4
#   float4(1,inf,inf,3)   ref {1,3,0,0} .xxyy    paints (1,1,3,3)
#   float4(-inf,1,-inf,3) ref {-inf,1,3,0} .xyzz paints (-inf,1,3,3)
#
# We do not chase that.  This row asserts OUR output, which the legacy
# packer produces identically, and which paints what the source says.
printf 'float4 main(float4 p : TEXCOORD0) : COLOR\n{\n    return float4(3.0e38 * 3.0e38, 1.0, 3.0e38 * 3.0e38, 2.0);\n}\n' \
    > "$work/repeated_inf.cg"
compile "$work/repeated_inf.cg" repeated_inf

# The two TRACKED fixtures this script has always used, kept so they stay
# exercised by name: both are byte-identical to the reference on the
# shipping path as of this slice.
broadcast="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_literal_broadcast_f.cg"
repeat="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_literal_repeat_f.cg"
[[ -f "$broadcast" ]] || fail "fixture missing: $broadcast"
[[ -f "$repeat" ]]    || fail "fixture missing: $repeat"
compile "$broadcast" broadcast
compile "$repeat" repeat

# THE DIFFERENTIAL CONTROL: one shape, the legacy path, same expectation.
# See the tombstone at the top of this file.
compile "$work/quarters.cg" quarters_legacy --legacy-lowering

# PREDICATION, read from the same containers the packing rows read.  A block
# only reaches a pixel through a write that actually executes; see the
# header for what the listing could not see.
check_unpredicated() {          # <tag>
    "$PYTHON" "$predication_checker" "$work/$1.fpo" > /dev/null ||
        fail "$1: a predicated or condition-forming write is present - the packed block below may never reach a pixel"
}

for shape in "${SHAPES[@]}"; do
    IFS='|' read -r tag _rest <<<"$shape"
    check_unpredicated "$tag"
done
for tag in repeated_inf broadcast repeat quarters_legacy; do
    check_unpredicated "$tag"
done

# THE ARTIFACT CONTROL: all1's own colour write, flipped to NVFX_COND_FL in
# the container bytes, must be rejected FOR THAT INSTRUCTION AND FOR THAT
# REASON - predication_check.py's own --control contract, which compares the
# reason exactly so a rejection for an unrelated field does not satisfy it.
"$PYTHON" "$predication_checker" "$work/all1.fpo" --control > /dev/null ||
    fail "all1: the COND_FL control was NOT rejected - the predication predicate is too weak to see a write that never executes"

"$PYTHON" - "$work" "$here" <<'PY'
import sys

work, here = sys.argv[1], sys.argv[2]
sys.path.insert(0, here)
import fp_sources  # noqa: E402

SHAPES = [
    ("all1", "3f800000 00000000 00000000 00000000", "xxxx"),
    ("bcast", "3ec00000 00000000 00000000 00000000", "xxxx"),
    ("half_pair", "3f800000 3f000000 00000000 00000000", "xyxy"),
    ("quarters", "3f000000 3e800000 3e000000 00000000", "xyxz"),
    ("all_distinct", "3f000000 3e800000 3e000000 3f800000", "xyzw"),
    ("zeros", "00000000 00000000 00000000 00000000", "xxxx"),
    ("zero_one", "00000000 3f800000 00000000 00000000", "xyxy"),
    ("adjacent_pairs", "40000000 40400000 00000000 00000000", "xxyy"),
    ("signed_pair", "bf800000 3f800000 00000000 00000000", "xyxy"),
    ("tail_run", "40e00000 3f800000 00000000 00000000", "xyyy"),
    ("three_values", "3f800000 40000000 40400000 00000000", "xyxz"),
    ("negzero_after", "00000000 00000000 00000000 00000000", "xxxx"),
    ("negzero_first", "80000000 00000000 00000000 00000000", "xxxx"),
    # The tracked fixtures, same rule.
    ("broadcast", "3f800000 00000000 00000000 00000000", "xxxx"),
    ("repeat", "3f800000 3f000000 00000000 00000000", "xyxy"),
    # OURS, not the reference's - see the named divergence above.
    ("repeated_inf", "7f800000 3f800000 40000000 00000000", "xyxz"),
    # The legacy path, on the shape the shipping path already asserted.
    ("quarters_legacy", "3f000000 3e800000 3e000000 00000000", "xyxz"),
]


def block_and_swizzle(tag):
    """The first inline const block and the swizzle THAT SLOT reads.

    The swizzle comes from the const slot itself and not from source 0: an
    instruction whose const operand is not its first would otherwise be
    judged on an unrelated slot's selector.
    """
    path = "%s/%s.fpo" % (work, tag)
    try:
        words = fp_sources.ucode_words(open(path, "rb").read())
        for w, block in fp_sources.instructions(words):
            if block is None:
                continue
            slot = next(s for s in (1, 2, 3)
                        if (w[s] & 3) == fp_sources.CONST)
            return (["%08x" % v for v in block],
                    fp_sources.swizzle_text((w[slot] >> 9) & 0xFF))
    except fp_sources.ContainerError as exc:
        raise SystemExit("FAIL %s: unreadable container: %s" % (tag, exc))
    raise SystemExit("FAIL %s: no inline const block in the program" % tag)


def mismatch(tag, block, swz, want_block, want_swz):
    """None when this is the reference's packing, a reason when it is not."""
    if block != want_block.split():
        return ("the const block is [%s]; the rule packs the DISTINCT values "
                "in first-appearance order, zero-filled, which is [%s]"
                % (" ".join(block), want_block))
    if swz != want_swz:
        return ("the block is packed correctly but read .%s instead of .%s - "
                "an identity swizzle over a packed block paints the wrong "
                "colour" % (swz, want_swz))
    return None


for tag, want_block, want_swz in SHAPES:
    block, swz = block_and_swizzle(tag)
    problem = mismatch(tag, block, swz, want_block, want_swz)
    if problem:
        raise SystemExit("FAIL %s: %s" % (tag, problem))

# CONTROLS.  Each feeds the comparison a shape that is wrong in exactly one
# way and requires it to be rejected FOR THAT WAY - a control that is
# rejected for the wrong reason proves nothing about the assertion.
packed, swizzle = "3f000000 3e800000 3e000000 00000000", "xyxz"

undeduped = ["3f000000", "3e800000", "3f000000", "3e000000"]
why = mismatch("control", undeduped, "xyzw", packed, swizzle)
if not why or "first-appearance order" not in why:
    raise SystemExit(
        "FAIL control: an UN-DEDUPED block read .xyzw - every lane written "
        "verbatim, which is the pre-slice shape - was not rejected for its "
        "packing: %r" % why)

why = mismatch("control", packed.split(), "xyzw", packed, swizzle)
if not why or "identity swizzle" not in why:
    raise SystemExit(
        "FAIL control: the RIGHT block read with an IDENTITY swizzle was not "
        "rejected for the swizzle: %r" % why)

reordered = ["3e000000", "3e800000", "3f000000", "00000000"]
why = mismatch("control", reordered, swizzle, packed, swizzle)
if not why or "first-appearance order" not in why:
    raise SystemExit(
        "FAIL control: the right SWIZZLE over a wrongly ORDERED block - the "
        "same three values, packed last-first - was not rejected: %r" % why)

print("  %d containers - the shapes, the tracked pair and the non-finite "
      "divergence - plus the legacy differential all match, none "
      "predicated; three packing controls and one COND_FL container "
      "control rejected"
      % len([s for s in SHAPES if not s[0].endswith("_legacy")]))
PY

printf 'literal-const-dedup-test: ok\n'
