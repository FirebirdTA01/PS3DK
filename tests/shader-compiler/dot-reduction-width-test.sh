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
# would satisfy a one-sided check.  The same stale-lane rule applies to
# stdlib lowerings that reduce through a dot: length, distance, and normalize.
# Scalar length and scalar distance must not dot at all, float2 must use DP2,
# float3 must use DP3, and float4 must use DP4.  sce-cgc exposes reflect()
# only on 3-wide vectors, so reflect2/reflect4 belong to overload parity
# rather than this dot-width fixture; fp_reflect3_width_f covers the accepted
# width touched by lowerReflect.
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

decode_fixture() {
    local stem="$1"
    local src="$shaders/$stem.cg"
    local log="$work/$stem.dump"
    local decoded="$work/$stem.decoded"
    [[ -f "$src" ]] || fail "fixture missing: $src"

    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --general-lowering "$src"
    ) >"$log" 2>&1 || rc=$?
    if [[ "$rc" -eq 124 ]]; then fail "$stem timed out"; fi
    if [[ "$rc" -ne 0 ]]; then
        tail -n 20 "$log" >&2
        fail "$stem did not compile on the general path"
    fi

    ( cd "$repo_root/tests/shader-compiler" \
      && python3 ucode_decode.py "$log" ) > "$decoded"

    [[ -s "$decoded" ]] || fail "$stem decoded no instructions"
}

check_dp2_fixture() {
    local stem="$1"
    local decoded="$work/$stem.decoded"
    decode_fixture "$stem"
    if ! grep -q ' 0x38 ' "$decoded"; then
        cat "$decoded" >&2
        fail "$stem emitted no DP2 (0x38).  A two-wide dot reduction lowered
as DP3 reads x*x + y*y plus a third lane outside the source value."
    fi
    if grep -q ' 0x05 ' "$decoded"; then
        cat "$decoded" >&2
        fail "$stem emitted a DP3 (0x05).  Every dot reduction in this
fixture is two components wide; a DP3 here is the stale-lane defect."
    fi
}

check_dp4_fixture() {
    local stem="$1"
    local decoded="$work/$stem.decoded"
    decode_fixture "$stem"
    if ! grep -q ' 0x06 ' "$decoded"; then
        cat "$decoded" >&2
        fail "$stem emitted no DP4 (0x06).  A four-wide reduction must include
the w lane rather than dropping it through DP3."
    fi
    if grep -q ' 0x05 ' "$decoded"; then
        cat "$decoded" >&2
        fail "$stem emitted a DP3 (0x05).  This fixture reduces a float4, so
DP3 silently drops w."
    fi
}

check_dp3_fixture() {
    local stem="$1"
    local decoded="$work/$stem.decoded"
    decode_fixture "$stem"
    if ! grep -q ' 0x05 ' "$decoded"; then
        cat "$decoded" >&2
        fail "$stem emitted no DP3 (0x05).  The accepted reflect width is
three components wide and should stay on the DP3 path."
    fi
    if grep -Eq ' 0x(06|38) ' "$decoded"; then
        cat "$decoded" >&2
        fail "$stem emitted DP2 or DP4.  This fixture pins reflect(float3),
the only reflect width sce-cgc accepts."
    fi
}

check_no_dot_fixture() {
    local stem="$1"
    local decoded="$work/$stem.decoded"
    decode_fixture "$stem"
    if grep -Eq ' 0x(05|06|38) ' "$decoded"; then
        cat "$decoded" >&2
        fail "$stem emitted a dot instruction.  Scalar length/distance is an
absolute value, not a reduction over unwritten lanes."
    fi
}

check_no_dot_fixture fp_length_scalar_f
check_no_dot_fixture fp_distance_scalar_f
check_dp2_fixture fp_dot2_f
check_dp2_fixture fp_length2_stale_z_f
check_dp2_fixture fp_distance2_stale_z_f
check_dp2_fixture fp_normalize2_stale_z_f
check_dp3_fixture fp_reflect3_width_f
check_dp4_fixture fp_length4_width_f
check_dp4_fixture fp_distance4_width_f
check_dp4_fixture fp_normalize4_width_f

printf 'PASS: dot-reduction-width-test\n'
