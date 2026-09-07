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

# A refusal is exit 1 EXACTLY.  124 is a timeout and >= 128 is a signal, and
# either one satisfies "did not exit 0" while meaning the compiler never
# reached the decision this guard is about - so a compiler that CRASHED on a
# shader it should have refused BY NAME was reported as correct here.  Call
# this wherever a compile's status is captured, whichever way that compile is
# expected to go: it is silent for 0 and for 1 and names anything else.
# Measured: half the guards in this suite that assert a refusal could not tell
# one from a SIGABRT (t_fd95d1b9).
refusal_status() {   # $1 rc, $2 what was compiled
    [[ "$1" -eq 124 ]] && fail "$2: the compiler timed out; a timeout is not a refusal"
    [[ "$1" -ge 128 ]] && fail "$2: the compiler died on signal $(( $1 - 128 )); a crash is not a refusal"
    [[ "$1" -eq 0 || "$1" -eq 1 ]] || fail "$2: the compiler exited $1; a refusal is exit 1"
    return 0
}

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
    local err="$work/$stem.err"
    [[ -f "$src" ]] || fail "fixture missing: $src"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$out" "$src"
    ) >"$log" 2>"$err" || {
        tail -n 30 "$err" >&2
        fail "$stem did not compile"
    }
    [[ -s "$out" ]] || fail "$stem did not emit a container"

    # stdout carries the ucode rows, stderr the diagnostics; merged, a
    # stderr line can land inside a hex row and cost it (the false R33,
    # 2026-09-07).  The decoder refuses such a log - this is why it does
    # not have to.
    local ucode_log="$work/$stem.ucode.log"
    local ucode_err="$work/$stem.ucode.err"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx "$src"
    ) >"$ucode_log" 2>"$ucode_err" || {
        tail -n 30 "$ucode_err" >&2
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
    local err="$work/$label.err"
    local rc=0
    rm -f "$out"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p "$profile" --emit-container "$out" "$src"
    ) >"$log" 2>"$err" || rc=$?

    refusal_status "$rc" "$label"
    [[ "$rc" -eq 1 ]] || fail "$label compiled; expected derivative refusal"
    [[ ! -e "$out" || ! -s "$out" ]] || fail "$label emitted a container after refusing"
    # the refusal text is on stderr; read both halves, because this asks
    # "did the compiler say it", not "on which stream did it say it"
    grep -Eqi "$needle" "$log" "$err" \
        || { tail -n 30 "$err" >&2; fail "$label refused for the wrong reason"; }
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
