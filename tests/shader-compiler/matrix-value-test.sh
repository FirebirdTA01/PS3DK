#!/usr/bin/env bash
# t_b4024c12: matrix values that are not bare uniforms must still lower.
# The fragment fixture constructs a float2x2 from scalars and immediately
# multiplies it by a vec2.  The vertex fixture multiplies two uniform mat4
# values, then uses the computed matrix in a matvec multiply.  Sony accepts
# both shapes; refusing them is the regression this guard pins.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-matrix-value-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
fp="$shaders/fp_matrix_constructor_f.cg"
vp="$shaders/vp_matrix_matmul_v.cg"
[[ -f "$fp" ]] || fail "fixture missing: $fp"
[[ -f "$vp" ]] || fail "fixture missing: $vp"

compile() {
    local tag="$1"
    local profile="$2"
    local src="$3"
    local log="$work/$tag.log"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p "$profile" "$src"
    ) >"$log" 2>&1 || {
        tail -n 30 "$log" >&2
        fail "$tag refused.  Matrix constructors, matrix products, and a computed matrix feeding matvec are reference-accepted shapes (t_b4024c12)."
    }
    grep -qE '^ +[0-9]+:' "$log" ||
        fail "$tag compiled but emitted no ucode"
}

compile fp_matrix_constructor sce_fp_rsx "$fp"
compile vp_matrix_matmul sce_vp_rsx "$vp"

cat >"$work/vp_mat3_row_xyz.cg" <<'SHADER'
void main(float3 p : POSITION, out float4 o : POSITION)
{
    float3x3 m = float3x3(p.xyz, float3(0.0, 1.0, 0.0), float3(0.0, 0.0, 1.0));
    float3 r = mul(m, float3(1.0, 0.0, 0.0));
    o = float4(r, 1.0);
}
SHADER

cat >"$work/vp_mat3_row_zyx.cg" <<'SHADER'
void main(float3 p : POSITION, out float4 o : POSITION)
{
    float3x3 m = float3x3(p.zyx, float3(0.0, 1.0, 0.0), float3(0.0, 0.0, 1.0));
    float3 r = mul(m, float3(1.0, 0.0, 0.0));
    o = float4(r, 1.0);
}
SHADER

compile vp_mat3_row_xyz sce_vp_rsx "$work/vp_mat3_row_xyz.cg"
compile vp_mat3_row_zyx sce_vp_rsx "$work/vp_mat3_row_zyx.cg"
grep -E '^ +[0-9]+:' "$work/vp_mat3_row_xyz.log" > "$work/vp_mat3_row_xyz.ucode"
grep -E '^ +[0-9]+:' "$work/vp_mat3_row_zyx.log" > "$work/vp_mat3_row_zyx.ucode"
if cmp -s "$work/vp_mat3_row_xyz.ucode" "$work/vp_mat3_row_zyx.ucode"; then
    cat "$work/vp_mat3_row_xyz.ucode" >&2
    fail "swizzled computed mat3 rows compiled to identical VP ucode.  DP3 legalization must preserve the row source swizzle rather than replacing it with xyz."
fi

cat >"$work/vp_mat2_matvec.cg" <<'SHADER'
void main(float2 p : POSITION, out float4 o : POSITION)
{
    float2x2 m = float2x2(1.0, 0.0, 0.0, 1.0);
    float2 r = mul(m, p);
    o = float4(r, 0.0, 1.0);
}
SHADER

mat2_out="$work/vp_mat2_matvec.vpo"
mat2_log="$work/vp_mat2_matvec.log"
mat2_rc=0
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
        -p sce_vp_rsx --emit-container "$mat2_out" "$work/vp_mat2_matvec.cg"
) >"$mat2_log" 2>&1 || mat2_rc=$?
[[ "$mat2_rc" -ne 0 ]] ||
    fail "vp_mat2_matvec compiled, but VP has no DP2 lowering in this slice"
[[ ! -e "$mat2_out" ]] ||
    fail "vp_mat2_matvec left a container behind after refusing"
grep -q "VP matvecmul with 2-column matrices is not implemented" "$mat2_log" ||
    { tail -n 20 "$mat2_log" >&2; fail "vp_mat2_matvec refused without the matrix-specific VP DP2 diagnostic"; }

printf 'matrix-value-test: ok\n'
