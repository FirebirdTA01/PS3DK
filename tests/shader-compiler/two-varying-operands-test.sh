#!/usr/bin/env bash
# No fragment instruction may name two input registers (t_e89cd261).
#
# An NV40 fragment instruction carries ONE input-source selector.  Two
# operands of register type INPUT therefore read the SAME varying,
# whatever the emitter meant, so `a - b` on two varyings emitted as a
# single ADD is `b - b` - zero, everywhere, with the container's input
# mask still naming both.  One operand has to be copied into a temp.
#
# The assertion is on the emitted words rather than an instruction
# sequence, so it survives the emitter improving.  But it is NOT a general
# rule, and the first version of this file said it was: two operands of
# type INPUT are perfectly legal when they name the SAME varying with
# different swizzles - `uv.x * uv.y` is one selector read twice, and the
# REFERENCE emits exactly that shape (a DP3 with two INPUT sources in
# fp_normalized_phong_vecinsert_f).  The encoding cannot tell the two
# cases apart, which is precisely why the defect was invisible.
#
# What makes the assertion sound HERE is the fixtures: their sources name
# two DIFFERENT varyings, so an instruction with two INPUT operands must
# have lost one of them.  A sweep of this check over the whole corpus is
# meaningless; over these two shaders it is exact.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-two-varying-operands-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
for f in fp_two_varyings_f.cg fp_two_varyings_mul_f.cg; do
    [[ -f "$shaders/$f" ]] || fail "fixture missing: $shaders/$f"
done

compile() {   # $1 fixture stem, $2 flags, $3 tag
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx ${2:+$2} "$shaders/$1.cg"
    ) >"$work/$3.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$3 timed out"
    if [[ "$rc" -ne 0 ]]; then
        tail -n 20 "$work/$3.log" >&2
        fail "$3 did not compile"
    fi
}

for stem in fp_two_varyings_f fp_two_varyings_mul_f; do
    compile "$stem" ""                   "${stem}_default"
    compile "$stem" --general-lowering   "${stem}_general"
done

python3 - "$work"/*.log <<'PY'
import re
import sys

REG_TYPE_INPUT = 1     # NVFX_FP_REG_TYPE_INPUT, the hardware encoding


def unswap(v):
    return ((v >> 16) | ((v & 0xFFFF) << 16)) & 0xFFFFFFFF


bad = []
seen = 0
for path in sys.argv[1:]:
    for line in open(path, "r", encoding="utf-8"):
        m = re.match(r"\s*(\d+):((?:\s+[0-9a-fA-F]{8})+)\s*$", line)
        if not m:
            continue
        words = [unswap(int(w, 16)) for w in m.group(2).split()]
        if len(words) < 4:
            continue
        # A const block is data, not an instruction; it has no opcode we
        # would recognise, and its "sources" are float bits.  Instructions
        # are the rows whose opcode field is a real opcode - every shape
        # here is MOV/ADD/MUL, so filter on the source register types
        # only for rows that carry an opcode in range.
        opcode = (words[0] >> 24) & 0x3F
        if opcode == 0 or opcode > 0x40:
            continue
        seen += 1
        inputs = [i for i in (1, 2, 3) if (words[i] & 3) == REG_TYPE_INPUT]
        if len(inputs) > 1:
            bad.append((path.split("\\")[-1].split("/")[-1],
                        m.group(1), opcode, len(inputs)))

if seen == 0:
    raise SystemExit(
        "FAIL: no instructions were examined, so the rule below was never "
        "applied - the ucode dump did not parse"
    )

if bad:
    lines = "\n".join(
        "  %s instruction %s (opcode 0x%x) names %d input registers"
        % b for b in bad
    )
    raise SystemExit(
        "FAIL: these fixtures combine two DIFFERENT varyings, and a fragment "
        "instruction has a single input-source selector - so an instruction "
        "with two INPUT operands here has lost one of them and reads the "
        "other twice (t_e89cd261).  Note the same shape is LEGAL when both "
        "operands name the same varying; it is these sources that make it a "
        "defect.  Offending instructions:\n" + lines
    )
PY

printf 'two-varying-operands-test: ok (rule checked on both paths)\n'
