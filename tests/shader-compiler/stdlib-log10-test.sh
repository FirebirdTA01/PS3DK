#!/usr/bin/env bash
# t_fe6d143b, small half: log10 is a registered builtin and must not fall
# through to IROp::Call.
#
# This deliberately does not solve user-function calls.  A source-defined
# function named log10 shadows the builtin overloads and must remain a user
# call until the frontend inliner lands; otherwise the small builtin fix would
# steal work from the larger call task.
#
# CONTROL: this fails on compilers before the log10 IR op because the builtin
# case reaches the general lowering as an unsupported call.
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

work="${TMPDIR:-/tmp}/ps3dk-stdlib-log10-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

cat >"$work/shadow_log10.fcg" <<'SHADER'
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

cat >"$work/builtin_log10_scalar.fcg" <<'SHADER'
void main(float x : TEXCOORD0, out float4 o : COLOR)
{
    float y = log10(abs(x) + 1.0);
    o = float4(y, y, y, 1.0);
}
SHADER

cat >"$work/builtin_log10_swizzle.fcg" <<'SHADER'
void main(float4 v : TEXCOORD0, out float4 o : COLOR)
{
    float3 y = log10(abs(v.wzy) + 1.0);
    o = float4(y, 1.0);
}
SHADER

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

shadow_log="$work/shadow_log10.log"
shadow_out="$work/shadow_log10.fpo"
rm -f "$shadow_out"
shadow_rc=0
compile_ir "shadow_log10" "$work/shadow_log10.fcg" "$shadow_out" "$shadow_log" || shadow_rc=$?
if [[ "$shadow_rc" -ne 0 ]]; then
    tail -n 20 "$shadow_log" >&2
    fail "user-defined log10 failed to compile"
fi
[[ -s "$shadow_out" ]] || fail "user-defined log10 did not emit a container"
grep -q 'define float @log10(float' "$shadow_log" \
    || fail "user-defined log10 body was not present in IR"
awk '
    /^define void @main/ { in_entry = 1 }
    in_entry { print }
    /^}/ && in_entry { exit }
' "$shadow_log" >"$work/shadow_log10.entry.ir"
if grep -Eq ' = call float .* @log10$' "$work/shadow_log10.entry.ir"; then
    fail "user-defined log10 call survived in main IR"
fi

run_builtin() {
    local name="$1"
    local src="$work/$name.fcg"
    local out="$work/$name.fpo"
    local log="$work/$name.log"
    rm -f "$out"
    rc=0
    compile_ir "$name" "$src" "$out" "$log" || rc=$?
    if [[ "$rc" -ne 0 ]]; then
        tail -n 20 "$log" >&2
        fail "$name failed to compile"
    fi
    [[ -s "$out" ]] || fail "$name did not emit a container"
    grep -Eq ' = log10 (float|vec[234]) ' "$log" \
        || fail "$name did not lower log10 to IR"
    if grep -Eq ' = call (float|vec[234]) .* @log10$' "$log"; then
        fail "$name left a log10 call in entry IR"
    fi

    local ucode_log="$work/$name.ucode.log"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --general-lowering "$src"
    ) >"$ucode_log" 2>&1 || {
        tail -n 20 "$ucode_log" >&2
        fail "$name failed while dumping ucode"
    }
    python3 "$repo_root/tests/shader-compiler/ucode_decode.py" "$ucode_log" \
        >"$work/$name.decode"
}

run_builtin builtin_log10_scalar
run_builtin builtin_log10_swizzle

python3 - "$work/builtin_log10_scalar.decode" "$work/builtin_log10_scalar.ucode.log" <<'PY'
import re
import sys

LG2, MUL, FENCBR = 0x1D, 0x02, 0x3E
CONST_BITS = "209b3e9a"  # printed form of 0x3e9a209b in the compiler dump

ops = []
for line in open(sys.argv[1], encoding="utf-8"):
    parts = line.split()
    if len(parts) < 2:
        continue
    try:
        ops.append(int(parts[1], 16))
    except ValueError:
        pass

if LG2 not in ops:
    raise SystemExit("FAIL: builtin log10 emitted no LG2 instruction")
if MUL not in ops:
    raise SystemExit("FAIL: builtin log10 emitted no scale MUL instruction")
if not any(ops[i] == LG2 and FENCBR in ops[i + 1:] and MUL in ops[i + 1:]
           for i in range(len(ops))):
    raise SystemExit("FAIL: builtin log10 did not emit LG2, then FENCBR, then MUL")

words = re.findall(r"\b[0-9a-fA-F]{8}\b", open(sys.argv[2], encoding="utf-8").read())
if CONST_BITS not in {w.lower() for w in words}:
    raise SystemExit("FAIL: builtin log10 did not emit scale constant 0x3e9a209b")
PY

printf 'stdlib-log10-test: ok\n'
