#!/usr/bin/env bash
# t_fe6d143b, large half: simple source-defined functions are inlined before
# the NV40 backend sees the IR.  A user call that survives to IROp::Call is a
# backend refusal today and a wrong abstraction layer: the reference compiler
# inlines these calls into the entry program.
#
# CONTROL: this fails on compilers before the inliner because inline_tint.fcg
# reaches the general lowering as "unsupported IR op call".
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

work="${TMPDIR:-/tmp}/ps3dk-user-function-inline-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

cat >"$work/inline_tint.fcg" <<'SHADER'
float3 tint(float3 c, float k)
{
    return c * k + 0.05;
}

void main(float4 color : TEXCOORD0, out float4 o : COLOR)
{
    float3 y = tint(color.xyz, color.w);
    o = float4(y, 1.0);
}
SHADER

cat >"$work/inline_shadow_builtin.fcg" <<'SHADER'
float log10(float x)
{
    return x + 2.0;
}

void main(float x : TEXCOORD0, out float4 o : COLOR)
{
    float y = log10(x);
    o = float4(y, y, y, 1.0);
}
SHADER

cat >"$work/inline_two_sites_swizzles.fcg" <<'SHADER'
float3 tint(float3 c, float k)
{
    return c * k + 0.05;
}

void main(float4 a : TEXCOORD0, float4 b : TEXCOORD1, out float4 o : COLOR)
{
    float3 x = tint(a.wzy, a.x);
    float3 y = tint(b.yxw, b.z);
    o = float4(x.x, y.y, x.z + y.z, 1.0);
}
SHADER

cat >"$work/inline_locals.fcg" <<'SHADER'
float3 shade(float3 c, float k)
{
    float t = max(0.0, k);
    float3 bias = float3(0.05, 0.10, 0.15);
    return c * t + bias;
}

void main(float4 color : TEXCOORD0, out float4 o : COLOR)
{
    float3 y = shade(color.zyx, color.w);
    o = float4(y, 1.0);
}
SHADER

cat >"$work/refuse_self_recursive.fcg" <<'SHADER'
float recur(float x)
{
    return recur(x);
}

void main(float x : TEXCOORD0, out float4 o : COLOR)
{
    float y = recur(x);
    o = float4(y, y, y, 1.0);
}
SHADER

cat >"$work/refuse_mutual_recursive.fcg" <<'SHADER'
float b(float x);

float a(float x)
{
    return b(x);
}

float b(float x)
{
    return a(x);
}

void main(float x : TEXCOORD0, out float4 o : COLOR)
{
    float y = a(x);
    o = float4(y, y, y, 1.0);
}
SHADER

cat >"$work/refuse_out_param.fcg" <<'SHADER'
void write_value(out float y, float x)
{
    y = x + 1.0;
}

void main(float x : TEXCOORD0, out float4 o : COLOR)
{
    float y = 0.0;
    write_value(y, x);
    o = float4(y, y, y, 1.0);
}
SHADER

cat >"$work/refuse_inout_param.fcg" <<'SHADER'
void adjust(inout float y)
{
    y = y + 1.0;
}

void main(float x : TEXCOORD0, out float4 o : COLOR)
{
    float y = x;
    adjust(y);
    o = float4(y, y, y, 1.0);
}
SHADER

cat >"$work/refuse_multi_return.fcg" <<'SHADER'
float pick(float x)
{
    if (x > 0.5)
        return x;
    return 1.0 - x;
}

void main(float x : TEXCOORD0, out float4 o : COLOR)
{
    float y = pick(x);
    o = float4(y, y, y, 1.0);
}
SHADER

write_chain_shader() {
    local path="$1" top="$2"
    {
        printf 'float f0(float x)\n{\n    return x + 0.125;\n}\n\n'
        for ((i = 1; i <= top; ++i)); do
            prev=$((i - 1))
            printf 'float f%d(float x)\n{\n    return f%d(x) + 0.125;\n}\n\n' "$i" "$prev"
        done
        printf 'void main(float x : TEXCOORD0, out float4 o : COLOR)\n{\n'
        printf '    float y = f%d(x);\n' "$top"
        printf '    o = float4(y, y, y, 1.0);\n}\n'
    } >"$path"
}

write_chain_shader "$work/inline_nested_limit.fcg" 15
write_chain_shader "$work/refuse_nested_past_limit.fcg" 16

compile_ir() {
    local label="$1" src="$2" out="$3" log="$4"
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --general-lowering --dump-ir \
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

expect_compile_without_call() {
    local name="$1"
    local src="$work/$name.fcg"
    local out="$work/$name.fpo"
    local log="$work/$name.log"
    rm -f "$out"
    local rc=0
    compile_ir "$name" "$src" "$out" "$log" || rc=$?
    if [[ "$rc" -ne 0 ]]; then
        tail -n 30 "$log" >&2
        fail "$name failed to compile"
    fi
    [[ -s "$out" ]] || fail "$name did not emit a container"
    awk '
        /^define void @main/ { in_entry = 1 }
        in_entry { print }
        /^}/ && in_entry { exit }
    ' "$log" >"$work/$name.entry.ir"
    if grep -Eq ' = call (float|vec[234]) ' "$work/$name.entry.ir"; then
        fail "$name left a user call in main IR"
    fi
}

expect_compile_without_call inline_tint
expect_compile_without_call inline_shadow_builtin
expect_compile_without_call inline_two_sites_swizzles
expect_compile_without_call inline_locals
expect_compile_without_call inline_nested_limit

expect_refusal() {
    local name="$1" needle="$2"
    local log="$work/$name.log"
    local out="$work/$name.fpo"
    rm -f "$out"
    local rc=0
    compile_ir "$name" "$work/$name.fcg" "$out" "$log" || rc=$?
    if [[ "$rc" -eq 0 ]]; then
        fail "$name compiled; expected a named inline refusal"
    fi
    grep -q "$needle" "$log" \
        || fail "$name did not report expected refusal: $needle"
    [[ ! -s "$out" ]] || fail "$name emitted a container after refusing"
}

expect_refusal refuse_multi_return "cannot inline user function 'pick'"
expect_refusal refuse_self_recursive "recursive user function call involving 'recur'"
expect_refusal refuse_mutual_recursive "recursive user function call involving 'a'"
expect_refusal refuse_out_param "cannot inline user function 'write_value'"
expect_refusal refuse_inout_param "cannot inline user function 'adjust'"
expect_refusal refuse_nested_past_limit "user function inline depth exceeded at 'f0'"

printf 'user-function-inline-test: ok\n'
