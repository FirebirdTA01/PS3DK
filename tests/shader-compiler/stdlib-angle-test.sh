#!/usr/bin/env bash
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

work="${TMPDIR:-/tmp}/ps3dk-shader-angle-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

cat >"$work/angle_scalar.fcg" <<'SHADER'
void main(float angle : TEXCOORD0, out float color : COLOR0)
{
    float r = radians(angle);
    float d = degrees(angle);
    color = r * 0.01 + d * 0.001;
}
SHADER

cat >"$work/angle_vector.fcg" <<'SHADER'
void main(float2 angle : TEXCOORD0, out float2 color : COLOR0)
{
    float2 r = radians(angle);
    float2 d = degrees(angle);
    color = r + d;
}
SHADER

cat >"$work/shadow_radians.fcg" <<'SHADER'
float radians(float x)
{
    return x;
}

void main(float angle : TEXCOORD0, out float color : COLOR0)
{
    color = radians(angle);
}
SHADER

run_case() {
    local name="$1"
    local src="$work/$name.fcg"
    local out="$work/$name.bin"
    local log="$work/$name.log"
    local rc=0

    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --general-lowering --dump-ir \
            --emit-container "$out" "$src"
    ) >"$log" 2>&1 || rc=$?

    if [[ "$rc" -eq 124 ]]; then
        fail "$name timed out; angle builtins must be bounded"
    fi
    if [[ "$rc" -eq 134 || "$rc" -eq 137 ]]; then
        fail "$name aborted or was killed under memory cap"
    fi
    if grep -Eq 'std::bad_alloc|terminate called|Aborted|Killed' "$log"; then
        fail "$name reported an allocation abort"
    fi
    if [[ "$rc" -ne 0 ]]; then
        tail -n 20 "$log" >&2
        fail "$name failed to compile"
    fi
    [[ -s "$out" ]] || fail "$name did not emit a container"
    grep -Eq ' = mul (float|vec[234]) ' "$log" \
        || fail "$name did not lower angle conversion to multiply IR"
    if grep -Eq ' = call (float|vec[234]) .* @(radians|degrees)$' "$log"; then
        fail "$name left an angle conversion call in entry IR"
    fi
}

run_case angle_scalar
run_case angle_vector

shadow_log="$work/shadow_radians.log"
shadow_out="$work/shadow_radians.bin"
shadow_rc=0
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
        -p sce_fp_rsx --general-lowering --dump-ir \
        --emit-container "$shadow_out" "$work/shadow_radians.fcg"
) >"$shadow_log" 2>&1 || shadow_rc=$?

if [[ "$shadow_rc" -eq 124 ]]; then
    fail "user-defined radians timed out"
fi
if [[ "$shadow_rc" -eq 134 || "$shadow_rc" -eq 137 ]]; then
    fail "user-defined radians aborted or was killed under memory cap"
fi
if grep -Eq 'std::bad_alloc|terminate called|Aborted|Killed' "$shadow_log"; then
    fail "user-defined radians reported an allocation abort"
fi
grep -q 'define float @radians(float' "$shadow_log" \
    || fail "user-defined radians body was not present in IR"
grep -Eq ' = call float .* @radians$' "$shadow_log" \
    || fail "user-defined radians call did not survive as a user call"

printf 'stdlib-angle-test: ok\n'
