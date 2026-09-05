#!/usr/bin/env bash
# vector-subscript-bounds-test.sh - constant vector indices must stay in range.
#
# This catches the silent-acceptance bug where float2 uv indexed as uv[7]
# passed semantic analysis and lowered to a container.  The safety sweep can be
# zero-mover even when this is broken, so the invalid cases below are the
# positive proof: each must refuse with a bounds diagnostic and leave no
# container behind.  Valid boundary indices compile first so a broken compiler
# build cannot pass every refusal trivially.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-vector-subscript-bounds-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

write_shader() {
    local name="$1" decl="$2" expr="$3"
    cat >"$work/$name.fcg" <<SHADER
void main($decl v : TEXCOORD0, out float4 o : COLOR)
{
    o = float4($expr, 0.0, 0.0, 1.0);
}
SHADER
}

run_compile() {
    local label="$1" src="$2" out="$3" log="$4"
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx \
            --emit-container "$out" "$src"
    ) >"$log" 2>&1 || rc=$?

    if [[ "$rc" -eq 124 ]]; then
        fail "$label timed out"
    fi
    if [[ "$rc" -eq 134 || "$rc" -eq 137 ]]; then
        fail "$label aborted or was killed under memory cap"
    fi
    return "$rc"
}

valid_cases=(
    "float2_low:float2:v[0] + v[1]"
    "float3_high:float3:v[2]"
    "float4_high:float4:v[3]"
)

for item in "${valid_cases[@]}"; do
    IFS=: read -r name decl expr <<<"$item"
    write_shader "$name" "$decl" "$expr"
    out="$work/$name.fpo"
    log="$work/$name.log"
    rm -f "$out"
    rc=0
    run_compile "$name" "$work/$name.fcg" "$out" "$log" || rc=$?
    [[ "$rc" -eq 0 ]] || { tail -n 20 "$log" >&2; fail "$name refused a valid vector subscript (rc=$rc)"; }
    [[ -s "$out" ]] || fail "$name produced no container"
done

cat >"$work/dynamic_in_range.fcg" <<'SHADER'
void main(float3 c : TEXCOORD0, out float4 o : COLOR)
{
    float s = 0.0;
    for (int i = 0; i < 3; i++)
    {
        s += c[i];
    }
    o = float4(s, c[1], 0.0, 1.0);
}
SHADER

out="$work/dynamic_in_range.fpo"
log="$work/dynamic_in_range.log"
rm -f "$out"
rc=0
run_compile "dynamic_in_range" "$work/dynamic_in_range.fcg" "$out" "$log" || rc=$?
[[ "$rc" -eq 0 ]] || { tail -n 20 "$log" >&2; fail "dynamic_in_range refused a runtime-bounded vector subscript (rc=$rc)"; }
[[ -s "$out" ]] || fail "dynamic_in_range produced no container"

invalid_cases=(
    "float2_seven:float2:v[7]"
    "float3_three:float3:v[3]"
    "float4_four:float4:v[4]"
    "float4_negative:float4:v[-1]"
)

for item in "${invalid_cases[@]}"; do
    IFS=: read -r name decl expr <<<"$item"
    write_shader "$name" "$decl" "$expr"
    out="$work/$name.fpo"
    log="$work/$name.log"
    rm -f "$out"
    rc=0
    run_compile "$name" "$work/$name.fcg" "$out" "$log" || rc=$?
    [[ "$rc" -ne 0 ]] || fail "$name accepted an out-of-range vector subscript"
    [[ ! -e "$out" ]] || fail "$name left a container behind after refusing"
    grep -Eqi 'array index out of bounds|vector index out of bounds|subscript.*out of bounds' "$log" \
        || { tail -n 20 "$log" >&2; fail "$name refused without a bounds diagnostic"; }
done

printf 'vector-subscript-bounds-test: ok\n'
