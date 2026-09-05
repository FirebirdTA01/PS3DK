#!/usr/bin/env bash
# The general path must legalise a COMPARISON's operands (t_40dd8159).
#
# Two NV40 fragment rules, both already implemented for arithmetic and
# both skipped for comparisons, because legalizeInputOperands consults a
# closed list of ops that did not contain them:
#
#   1. ONE input-source selector per instruction.  The selector is a field
#      of the INSTRUCTION (NVFX_FP_OP_INPUT_SRC, hw[0] bits 13..16), not
#      of the source, so two input-typed operands necessarily read the
#      SAME varying: `c.w < d.w` on two varyings compiled to
#      `SLT ..., f[TEX1].w, f[TEX1].w`, d.w compared with itself
#      (t_e89cd261's rule, fixed for arithmetic in cf07e6c).
#
#   2. ONE inline constant block per instruction.  A comparison of a
#      uniform against a literal appended a block for each after the same
#      instruction, so the second was decoded as an instruction and the
#      operand resolved to the first block.
#
# Both are asserted on the UCODE, decoded, and never on a mask or a
# diagnostic: a container can name an input that no instruction reads,
# which is how the first defect hid for as long as it did.
#
# Rule 1 is asserted HERE and not as a sweep on purpose.  The ucode cannot
# tell "two operands that legitimately name the same varying" from "two
# operands that were meant to be different", so a corpus sweep for it
# reports correct shaders as failures.  On a fixture whose source names
# two different varyings, the question has an answer.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-general-operand-legalisation.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
for f in fp_cmp_two_varyings_f.cg fp_cmp_uniform_literal_f.cg; do
    [[ -f "$shaders/$f" ]] || fail "fixture missing: $shaders/$f"
done

dump() {   # $1 stem, $2 tag
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx "$shaders/$1.cg"
    ) >"$work/$2.dump" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$2 timed out"
    if [[ "$rc" -ne 0 ]]; then
        tail -n 20 "$work/$2.dump" >&2
        fail "$2 did not compile on the general path"
    fi
}

dump fp_cmp_two_varyings_f    two_varyings
dump fp_cmp_uniform_literal_f uniform_literal

python3 "$repo_root/tests/shader-compiler/ucode_operand_rules.py" \
    "$work/two_varyings.dump" "$work/uniform_literal.dump"

printf 'PASS: general-operand-legalisation-test\n'
