#!/usr/bin/env bash
# t_b28f9994 / t_6f5f1694: screen-space derivative slice 1.
#
# The shipping path must accept scalar and float2 fragment ddx/ddy and emit
# NV40's native DDX/DDY opcodes.  Widths 3/4 are deliberately out of scope for
# slice 1 because the hardware writes only X/Y; they must refuse by name rather
# than silently truncating.  Vertex derivatives are a profile error.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-stdlib-derivatives-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"

compile_fp() {
    local stem="$1"
    local src="$shaders/$stem.cg"
    local out="$work/$stem.fpo"
    local log="$work/$stem.log"
    [[ -f "$src" ]] || fail "fixture missing: $src"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$out" "$src"
    ) >"$log" 2>&1 || {
        tail -n 30 "$log" >&2
        fail "$stem did not compile"
    }
    [[ -s "$out" ]] || fail "$stem did not emit a container"

    local ucode_log="$work/$stem.ucode.log"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx "$src"
    ) >"$ucode_log" 2>&1 || {
        tail -n 30 "$ucode_log" >&2
        fail "$stem failed while dumping ucode"
    }
    python3 "$repo_root/tests/shader-compiler/ucode_decode.py" "$ucode_log" \
        >"$work/$stem.decode"
}

expect_refusal() {
    local label="$1"
    local profile="$2"
    local needle="$3"
    local src="$4"
    local out="$work/$label.bin"
    local log="$work/$label.log"
    local rc=0
    rm -f "$out"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p "$profile" --emit-container "$out" "$src"
    ) >"$log" 2>&1 || rc=$?

    [[ "$rc" -ne 0 ]] || fail "$label compiled; expected derivative refusal"
    [[ ! -e "$out" || ! -s "$out" ]] || fail "$label emitted a container after refusing"
    grep -Eqi "$needle" "$log" \
        || { tail -n 30 "$log" >&2; fail "$label refused for the wrong reason"; }
}

compile_fp fp_deriv_scalar_f
compile_fp fp_deriv_float2_f

python3 - \
    "$work/fp_deriv_scalar_f.decode" \
    "$work/fp_deriv_float2_f.decode" <<'PY'
import sys

DDX = 0x15
DDY = 0x16

for path in sys.argv[1:]:
    rows = []
    for line in open(path, encoding="utf-8"):
        parts = line.split()
        if len(parts) < 8:
            continue
        try:
            op = int(parts[1], 16)
        except ValueError:
            continue
        mask = None
        for part in parts:
            if part.startswith("mask=0x"):
                mask = int(part.split("=", 1)[1], 16)
                break
        rows.append((op, mask))

    ops = [op for op, _ in rows]
    if DDX not in ops:
        raise SystemExit(f"FAIL: {path} emitted no DDX opcode")
    if DDY not in ops:
        raise SystemExit(f"FAIL: {path} emitted no DDY opcode")
    for op, mask in rows:
        if op in (DDX, DDY):
            if mask is None or mask == 0:
                raise SystemExit(f"FAIL: {path} derivative opcode has empty mask")
            if mask & ~0x3:
                raise SystemExit(f"FAIL: {path} derivative opcode writes non-XY mask 0x{mask:x}")
PY

expect_refusal "fp_deriv_float3" sce_fp_rsx \
    "derivative width .*not yet supported" \
    "$shaders/fp_deriv_float3_refuse_f.cg"
expect_refusal "fp_deriv_float4" sce_fp_rsx \
    "derivative width .*not yet supported" \
    "$shaders/fp_deriv_float4_refuse_f.cg"
expect_refusal "vp_deriv" sce_vp_rsx \
    "derivative.*fragment" \
    "$shaders/vp_deriv_refuse_v.cg"

printf 'stdlib-derivatives-test: ok\n'
