#!/usr/bin/env bash
# A vector-valued if/else merge of two RAW INPUTS keeps its width (t_7b20ffdc).
#
# `float4 r; if (c.x > k) r = c; else r = d; o = r;` with c and d function
# parameters.  The IR builder typed that merge's Select from the
# then-value through a lookup that never consulted parameters, so the
# Select was Void, and the general path's predicated select masked its
# write to lane x and broadcast the lane into every channel: every
# channel of the merge wrong, the shader compiling and exiting 0.  The
# reference writes all four lanes on every value move of that merge.
#
# The assertion is on the emitted words, built from the reference's
# behaviour: every MOV whose source is an INPUT register writes the full
# mask.  In this fixture the only MOVs that read an input are the two
# arm moves (the comparison is an SGT, not a MOV), so the rule is exact
# here and NOT a general one - a shader that legitimately moves one lane
# of an input would violate it.  The DEFAULT path refuses this shape
# outright ("Select: two varying branches not yet supported", measured on
# f021170 and after the fix alike) - an honest refusal, accepted here by
# its message; should the matcher ever learn the shape, its words are
# held to the same full-mask rule.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"
work="${TMPDIR:-/tmp}/ps3dk-select-merge-inputs-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT
shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
stem=fp_select_merge_inputs_f
[[ -f "$shaders/$stem.cg" ]] || fail "fixture missing: $shaders/$stem.cg"

compile() {   # $1 flags, $2 tag
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx ${1:+$1} "$shaders/$stem.cg"
    ) >"$work/$2.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$2 timed out"
    if [[ "$rc" -ne 0 ]]; then
        tail -n 20 "$work/$2.log" >&2
        fail "$2 did not compile"
    fi
}
compile --general-lowering  "${stem}_general"

# Default path: a refusal is accepted only with the measured message; a
# compile is held to the same rule as the general path.
logs=("$work/${stem}_general.log")
rc=0
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
        -p sce_fp_rsx "$shaders/$stem.cg"
) >"$work/${stem}_default.log" 2>&1 || rc=$?
[[ "$rc" -eq 124 ]] && fail "${stem}_default timed out"
if [[ "$rc" -eq 0 ]]; then
    logs+=("$work/${stem}_default.log")
elif ! grep -q "Select: two varying branches not yet supported" "$work/${stem}_default.log"; then
    tail -n 20 "$work/${stem}_default.log" >&2
    fail "${stem}_default failed for a reason other than the measured refusal"
fi

python3 - "${logs[@]}" <<'PY'
import re
import sys
REG_TYPE_INPUT = 1     # NVFX_FP_REG_TYPE_INPUT
REG_TYPE_CONST = 2     # NVFX_FP_REG_TYPE_CONST: an inline const block follows
OPCODE_MOV = 0x01
def unswap(v):
    return ((v >> 16) | ((v & 0xFFFF) << 16)) & 0xFFFFFFFF
bad = []
seen_input_movs = {}
for path in sys.argv[1:]:
    tag = path.split("\\")[-1].split("/")[-1]
    seen_input_movs[tag] = 0
    skip_next = False
    for line in open(path, "r", encoding="utf-8"):
        m = re.match(r"\s*(\d+):((?:\s+[0-9a-fA-F]{8})+)\s*$", line)
        if not m:
            continue
        words = [unswap(int(w, 16)) for w in m.group(2).split()]
        if len(words) < 4:
            continue
        # An inline const block is 16 bytes of DATA following the
        # instruction that names it (a source of register type CONST, 2);
        # decoding it as an instruction is the shape of a false red
        # (claude, review of d5fda09).  Skip it the way the guest's
        # measure_cost does.
        if skip_next:
            skip_next = False
            continue
        skip_next = any((words[i] & 3) == REG_TYPE_CONST for i in (1, 2, 3))
        opcode = (words[0] >> 24) & 0x3F
        if opcode != OPCODE_MOV:
            continue
        # Word 0: destination register at bits 1..6 (0x3F = none, the
        # CC-only write), write mask at bits 9..12.  Word 1: source 0,
        # register type at bits 0..1.
        dest = (words[0] >> 1) & 0x3F
        mask = (words[0] >> 9) & 0xF
        if dest == 0x3F:
            continue
        if (words[1] & 3) != REG_TYPE_INPUT:
            continue
        seen_input_movs[tag] += 1
        if mask != 0xF:
            bad.append((tag, m.group(1), mask))
for tag, n in seen_input_movs.items():
    if n == 0:
        raise SystemExit(
            "FAIL: %s: no MOV reading an input register was found, so the "
            "rule was never applied - the ucode dump did not parse or the "
            "emitter changed the merge's shape; re-derive the assertion "
            "from the reference before relaxing it" % tag
        )
if bad:
    lines = "\n".join(
        "  %s instruction %s writes mask 0x%x" % b for b in bad
    )
    raise SystemExit(
        "FAIL: this fixture merges two float4 INPUTS, and the reference "
        "writes all four lanes on every value move of that merge; a MOV "
        "from an input with a partial mask here is the merge collapsing "
        "to one lane (t_7b20ffdc).  Offending instructions:\n" + lines
    )
PY
if [[ ${#logs[@]} -eq 2 ]]; then
    printf 'select-merge-inputs-test: ok (full-width merge on both paths)\n'
else
    printf 'select-merge-inputs-test: ok (full-width merge on the general path; the default path refuses the shape as measured)\n'
fi
