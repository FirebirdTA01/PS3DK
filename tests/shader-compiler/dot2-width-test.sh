#!/usr/bin/env bash
# A 2-component dot product is DP2, not DP3 (t_a30159bf).
#
# The general path chose DP4 for operand width >= 4 and DP3 for everything
# else, so `dot(v, v)` on a float2 read a THIRD lane that nothing wrote:
# the multiply before it carries a two-lane write mask, and whatever the
# allocator left in z went into the sum.  Not reliably wrong, which is the
# bad kind - the same source is right in one shader and wrong in the next.
#
# The reference emits DP2 here; NV40 has the opcode (0x38) and we simply
# had no VOp for it.  Asserted on the decoded ucode, both directions: DP2
# must be there AND DP3 must not, because a lowering that emitted both
# would satisfy a one-sided check.
#
# Fragment only.  NV40's vertex unit has no DP2 - the reference expands a
# vertex dot2 into MUL + ADD - and vpOpcode's fallthrough is MOV, which
# would be a silent wrong answer.  The vertex side of the same question is
# its own item and is deliberately not asserted here.
#
# The default path refuses this shader for unrelated reasons, so there is
# no default column: a test that asserted one would be asserting the
# matcher's coverage, not the dot's width.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-dot2-width.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
[[ -f "$shaders/fp_dot2_f.cg" ]] || fail "fixture missing: $shaders/fp_dot2_f.cg"

rc=0
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
        -p sce_fp_rsx --general-lowering "$shaders/fp_dot2_f.cg"
) >"$work/dot2.dump" 2>&1 || rc=$?
if [[ "$rc" -eq 124 ]]; then fail "fp_dot2_f timed out"; fi
if [[ "$rc" -ne 0 ]]; then
    tail -n 20 "$work/dot2.dump" >&2
    fail "fp_dot2_f did not compile on the general path"
fi

decoded="$work/dot2.decoded"
( cd "$repo_root/tests/shader-compiler" \
  && python3 ucode_decode.py "$work/dot2.dump" ) > "$decoded"

[[ -s "$decoded" ]] || fail "fp_dot2_f decoded no instructions"

if ! grep -q ' 0x38 ' "$decoded"; then
    cat "$decoded" >&2
    fail "fp_dot2_f emitted no DP2 (0x38).  A 2-wide dot lowered as DP3
reads a third lane the multiply before it never wrote - x*x + y*y plus
whatever the allocator left in z (t_a30159bf)."
fi
if grep -q ' 0x05 ' "$decoded"; then
    cat "$decoded" >&2
    fail "fp_dot2_f emitted a DP3 (0x05).  The only dot in this shader is
two components wide; a DP3 here is the stale-lane defect."
fi

printf 'PASS: dot2-width-test\n'
