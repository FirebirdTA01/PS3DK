#!/usr/bin/env bash
# A literal division by zero is THREE cases and the reference folds only one
# of them (t_3ff60769).
#
# THE RULE, read off sce-cgc 475 by printing every instruction and every
# const block of its container - not inferred from one probe:
#
#   every denominator non-zero                     fold per lane
#   every denominator zero AND every numerator      fold each lane to its
#     zero                                          NUMERATOR, sign preserved
#   anything else                                   DO NOT FOLD; the
#                                                   reference emits a runtime
#                                                   reciprocal
#
# THE SIGN IS THE POINT, and it is why the obvious implementation is wrong.
# 0/0 does not fold to +0: it folds to the numerator, so -0.0/0.0 gives -0.0
# and 0.0/-0.0 gives +0.0.  A "fold 0/0 to zero" patch passes the plain row
# and fails the two signed ones, which is exactly what they are here for.
#
# THE THIRD CASE IS WHY THIS IS NOT "RELAX THE GUARD".  ir_builder.cpp used
# to return InvalidIRValue for ANY zero denominator - right for x/0, wrong
# for 0/0.  Dropping it altogether would fold x/0 too, which the reference
# does not do, and would fold a MIXED denominator vector, which it also does
# not do: float4(0,0,2,3) / float4(0,0,2,1) is emitted as RCP/MUL even though
# every zero-denominator lane there has a zero numerator.  The two rows at
# the end require a runtime reciprocal to still be present.
#
# WHAT THIS FILE DOES NOT ASSERT: the SHAPE of the unfolded cases.  We spend
# five instructions and four blocks on float4(1.0/0.0, 1, 3, 2) where the
# reference spends two and two, because we write one constant lane per MOV
# instead of masking three into one.  That is t_49265c44 and it is a
# different subject; these rows only require that the division did not fold.
#
# THE ARTIFACT IS THE CONTAINER, for the values and for the predication
# alike, and every expected block below was read off the REFERENCE's own
# container.
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

work="${TMPDIR:-/tmp}/ps3dk-const-division-fold-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

compile() {   # <tag> <body>
    local tag="$1" body="$2" rc=0
    printf 'float4 main(float4 p : TEXCOORD0) : COLOR\n{\n    %s\n}\n' \
        "$body" > "$work/$tag.cg"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$work/$tag.fpo" "$work/$tag.cg"
    ) >"$work/$tag.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$tag timed out"
    if [[ "$rc" -ne 0 ]]; then
        tail -n 20 "$work/$tag.log" >&2
        fail "$tag failed to compile"
    fi
    [[ -s "$work/$tag.fpo" ]] || fail "$tag.fpo is missing or empty"
}

# FOLDED rows: tag|body|expected block words|expected swizzle of the const
# source.  All read off the reference.
compile plain     "return float4(0.0/0.0, 1.0, 3.0, 2.0);"
compile neg_num   "return float4(-0.0/0.0, 1.0, 3.0, 2.0);"
compile neg_den   "return float4(0.0/-0.0, 1.0, 3.0, 2.0);"
compile both_neg  "return float4(-0.0/-0.0, 1.0, 3.0, 2.0);"
compile ordinary  "return float4(1.0/2.0, 1.0, 3.0, 2.0);"
compile zero_num  "return float4(0.0/2.0, 1.0, 3.0, 2.0);"
compile vec_signs "return float4(0.0, -0.0, 0.0, -0.0) / float4(0.0, 0.0, -0.0, -0.0);"

# UNFOLDED rows: the reference leaves these as runtime reciprocals.
compile x_over_0  "return float4(1.0/0.0, 1.0, 3.0, 2.0);"
compile vec_mixed "return float4(0.0, 0.0, 2.0, 3.0) / float4(0.0, 0.0, 2.0, 1.0);"

for tag in plain neg_num neg_den both_neg ordinary zero_num vec_signs \
           x_over_0 vec_mixed; do
    "$PYTHON" "$predication_checker" "$work/$tag.fpo" > /dev/null ||
        fail "$tag: a predicated or condition-forming write is present - the value below may never reach a pixel"
done
"$PYTHON" "$predication_checker" "$work/plain.fpo" --control > /dev/null ||
    fail "plain: the COND_FL control was NOT rejected - the predication predicate is too weak"

# THE VALUE, computed from the program - not read off the block.  The rows
# below also assert the block and the swizzle, which localise a failure, but
# THIS is the binding assertion: codex flipped SRC0's negate bit on `plain`'s
# folded MOV, leaving the block, the swizzle, the status and stdout untouched,
# and every row and self-check in this file stayed green while the shader
# painted (-0,-1,-3,-2).  A block says what is STORED.  Only walking the
# program says what is WRITTEN.
value_checker="$here/fp_const_output_check.py"
[[ -f "$value_checker" ]] || fail "fp_const_output_check.py is missing"
paints() {   # <tag> <four expected R0 words>
    local tag="$1"; shift
    "$PYTHON" "$value_checker" "$work/$tag.fpo" "$@" > /dev/null ||
        fail "$tag: the colour output is not what the reference paints (run $value_checker for the lane diff)"
}
paints plain     00000000 3f800000 40400000 40000000
paints neg_num   80000000 3f800000 40400000 40000000
paints neg_den   00000000 3f800000 40400000 40000000
paints both_neg  80000000 3f800000 40400000 40000000
paints ordinary  3f000000 3f800000 40400000 40000000
paints zero_num  00000000 3f800000 40400000 40000000
# The vector sign-merge PAINTS four lanes of +0: the numerator rule gives
# (+0,-0,+0,-0) and the packing merges them to one representative read .xxxx,
# so every lane reads the FIRST one, which is +0.  Asserting the block alone
# could not tell that from painting -0.
paints vec_signs 00000000 00000000 00000000 00000000

# THE ARTIFACT CONTROLS, on this row's own container: each mutation must be
# REJECTED by the same evaluator, or the rows above are decoration.  negate is
# codex's first; scale (a 2X destination scale) is his second, and it is why
# this evaluator now shares inline_factor_check's mode table instead of
# carrying its own list; swizzle rewrites the read to .xxxx; mask drops the
# write to one lane; precision sets a non-fp32 precision; no-end clears END.
for control in negate swizzle mask scale precision no-end; do
    "$PYTHON" "$value_checker" "$work/plain.fpo"         00000000 3f800000 40400000 40000000 --control "$control" > /dev/null ||
        fail "plain: the $control control was NOT rejected - the value check is too weak to see it"
done

"$PYTHON" - "$work" "$here" <<'PY'
import sys

work, here = sys.argv[1], sys.argv[2]
sys.path.insert(0, here)
import fp_sources  # noqa: E402

DIV, RCP = 0x3A, 0x1A


def program(tag):
    blob = open("%s/%s.fpo" % (work, tag), "rb").read()
    try:
        return list(fp_sources.instructions(fp_sources.ucode_words(blob)))
    except fp_sources.ContainerError as exc:
        raise SystemExit("FAIL %s: unreadable container: %s" % (tag, exc))


def opcodes(ins):
    return [(w[0] >> 24) & 0x3F for w, _ in ins]


def folded_block(ins):
    """The first inline const block and the swizzle THAT slot reads."""
    for w, block in ins:
        if block is None:
            continue
        slot = next(s for s in (1, 2, 3) if (w[s] & 3) == fp_sources.CONST)
        return (["%08x" % v for v in block],
                fp_sources.swizzle_text((w[slot] >> 9) & 0xFF))
    return None, None


# THE PREDICATES BELOW TAKE VALUES, NOT TAGS, so the self-checks at the end
# exercise THE SAME code the rows do.  A self-check written against a second
# copy of the comparison would only prove the copy agrees with itself.
def reciprocal_reason(ops):
    """A reason when a runtime reciprocal is present, else None."""
    if DIV in ops:
        return "the program still contains a runtime DIV"
    if RCP in ops:
        return "the program still contains a runtime RCP"
    return None


def block_reason(block, swz, want_block, want_swz):
    """A reason when the folded constant is not the reference's, else None."""
    if block is None:
        return "no inline const block: nothing carries the folded value"
    if block != want_block.split():
        return ("the folded block is [%s]; 0/0 folds to the NUMERATOR with "
                "its sign, which makes it [%s]" % (" ".join(block), want_block))
    if swz != want_swz:
        return ("the block is right but read .%s instead of .%s"
                % (swz, want_swz))
    return None


def check_folded(tag, want_block, want_swz):
    ins = program(tag)
    left = reciprocal_reason(opcodes(ins))
    if left:
        return left + ", so the constant division did not fold"
    block, swz = folded_block(ins)
    return block_reason(block, swz, want_block, want_swz)


def check_unfolded(tag):
    if reciprocal_reason(opcodes(program(tag))):
        return None
    return ("the division was FOLDED; the reference leaves this one as a "
            "runtime reciprocal, so folding it is a divergence introduced by "
            "relaxing the zero-denominator guard too far")


FOLDED = [
    # tag,        block,                                 swizzle
    ("plain",     "00000000 3f800000 40400000 40000000", "xyzw"),
    ("neg_num",   "80000000 3f800000 40400000 40000000", "xyzw"),
    ("neg_den",   "00000000 3f800000 40400000 40000000", "xyzw"),
    ("both_neg",  "80000000 3f800000 40400000 40000000", "xyzw"),
    ("ordinary",  "3f000000 3f800000 40400000 40000000", "xyzw"),
    ("zero_num",  "00000000 3f800000 40400000 40000000", "xyzw"),
    # The numerator rule at vector width: the lanes are (+0,-0,+0,-0) and the
    # const-block packing then merges them because they compare equal, so one
    # representative is read .xxxx (t_642eb36e).
    ("vec_signs", "00000000 00000000 00000000 00000000", "xxxx"),
]

for tag, block, swz in FOLDED:
    problem = check_folded(tag, block, swz)
    if problem:
        raise SystemExit("FAIL %s: %s" % (tag, problem))

for tag in ("x_over_0", "vec_mixed"):
    problem = check_unfolded(tag)
    if problem:
        raise SystemExit("FAIL %s: %s" % (tag, problem))

# SELF-CHECKS.  Each feeds a predicate the shape a plausible WRONG
# implementation would produce and requires rejection FOR THAT REASON.  Rows
# that all pass on the code that exists say nothing about the code that might
# replace it.
SIGNED = "80000000 3f800000 40400000 40000000"
PLUS_ZERO = ["00000000", "3f800000", "40400000", "40000000"]

# 1. "fold 0/0 to +0" - the obvious wrong patch.  It passes `plain` and must
#    fail the signed rows, so the two expectations have to differ at all.
if PLUS_ZERO == SIGNED.split():
    raise SystemExit("FAIL self-check: the signed expectation is not "
                     "distinguishable from folding to +0")
reason = block_reason(PLUS_ZERO, "xyzw", SIGNED, "xyzw")
if not reason or "NUMERATOR" not in reason:
    raise SystemExit("FAIL self-check: a +0 where -0 is required was not "
                     "rejected for the numerator rule: %r" % reason)

# 2. The right block read with the wrong swizzle.
reason = block_reason(SIGNED.split(), "xxxx", SIGNED, "xyzw")
if not reason or "read .xxxx" not in reason:
    raise SystemExit("FAIL self-check: the right block read with the wrong "
                     "swizzle was not rejected for the swizzle: %r" % reason)

# 3. No const block at all.
reason = block_reason(None, None, SIGNED, "xyzw")
if not reason or "no inline const block" not in reason:
    raise SystemExit("FAIL self-check: a program with no const block was not "
                     "rejected: %r" % reason)

# 4. The unfolded rows must be able to fail: "relaxed the guard too far" looks
#    like a program with neither DIV nor RCP.  0x01 is MOV.
if reciprocal_reason([0x01, 0x01]) is not None:
    raise SystemExit("FAIL self-check: a program of plain MOVs was reported "
                     "as carrying a runtime reciprocal")
if reciprocal_reason([0x01, DIV]) is None or reciprocal_reason([0x01, RCP]) is None:
    raise SystemExit("FAIL self-check: a program carrying DIV or RCP was not "
                     "recognised as unfolded")

print("  %d folded rows including both signed-zero orders and the vector "
      "sign-merge, 2 rows required to stay unfolded, four self-checks "
      "rejected, none predicated" % len(FOLDED))
PY

printf 'const-division-fold-test: ok\n'
