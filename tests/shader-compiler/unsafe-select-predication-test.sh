#!/usr/bin/env bash
# A straight-line source-level ternary with an unsafe arm must lower through
# the predicated-select path, not through arithmetic blending.
#
# Arithmetic select computes `dst = cond * (then - else) + else`.  That is
# valid only when both arms are safe to evaluate and blend numerically.  If an
# arm can produce inf/NaN, `0 * NaN` can poison the selected value even when
# that arm is not taken.  The control-flow flattener already uses the
# predicated path for unsafe join selects; this test pins the same rule for a
# one-block `?:`.
#
# CONTROL: this fails on the first user-function inliner binary because it
# emits a MAD blend for this witness and paints test_01_rsxrt wrong after the
# inliner admits it.  It passes once unsafe fragment selects emit a condition
# write followed by a predicated MOV.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-unsafe-select-predication-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

cat >"$work/unsafe_select.fcg" <<'SHADER'
void main(float x : TEXCOORD0, out float4 o : COLOR)
{
    float denom = x - x;
    float y = (x > 0.5) ? 0.25 : (1.0 / denom);
    o = float4(y, y, y, 1.0);
}
SHADER

(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
        -p sce_fp_rsx --general-lowering --dump-ir "$work/unsafe_select.fcg"
) >"$work/unsafe_select.log" 2>&1 || {
    tail -n 30 "$work/unsafe_select.log" >&2
    fail "unsafe select witness did not compile"
}

python3 "$repo_root/tests/shader-compiler/ucode_decode.py" \
    "$work/unsafe_select.log" >"$work/unsafe_select.decode"

python3 - "$work/unsafe_select.decode" <<'PY'
import re
import sys

rows = []
for line in open(sys.argv[1], encoding="utf-8"):
    m = re.match(r"(\d+) 0x([0-9A-Fa-f]+).*ccw=(\d).*cc=([A-Z]+).*none=(\d).*mask=0x([0-9A-Fa-f]+)", line)
    if m:
        rows.append({
            "index": int(m.group(1)),
            "op": int(m.group(2), 16),
            "ccw": int(m.group(3)),
            "cc": m.group(4),
            "none": int(m.group(5)),
            "mask": int(m.group(6), 16),
        })

if not rows:
    raise SystemExit("FAIL: the witness ucode dump did not decode")

if any(r["op"] == 0x04 for r in rows):
    raise SystemExit(
        "FAIL: unsafe select lowered as an arithmetic MAD blend.  A non-finite "
        "untaken arm can poison that blend; it must use predicated MOV "
        "(t_fe6d143b).")

cc_writes = [r for r in rows if r["ccw"] == 1 and r["none"] == 1]
pred_moves = [r for r in rows if r["op"] == 0x01 and r["cc"] == "NE"]
if not cc_writes:
    raise SystemExit("FAIL: unsafe select emitted no condition-code write")
if not pred_moves:
    raise SystemExit("FAIL: unsafe select emitted no NE-predicated MOV")

print("unsafe-select-predication: %d CC write(s), %d predicated MOV(s)"
      % (len(cc_writes), len(pred_moves)))
PY

printf 'unsafe-select-predication-test: ok\n'
