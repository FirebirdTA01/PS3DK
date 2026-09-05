#!/usr/bin/env bash
# t_582a7fff: The FP scheduler must model WAW dependency edges for output register
# destinations (kOutputKeyBase), keeping repeated stores to one output in source order.
#
# Witness fixture: fp_output_stored_thrice_f.cg (t_c2582cf1 / t_becbfa69).
# Three sequential stores to output colour R0 set alpha constants:
#   1.0f (0x3f800000), 0.5f (0x3f000000), 0.25f (0x3e800000).
# Under the defect, missing WAW edges on output destinations caused the scheduler's
# latest-in-program-order tie break to emit the stores backwards (0.25f first, 1.0f last).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-scheduler-output-waw-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
src="$shaders/fp_output_stored_thrice_f.cg"
[[ -f "$src" ]] || fail "fixture missing: $src"

log="$work/fp_output_stored_thrice.log"
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
        -p sce_fp_rsx "$src"
) >"$log" 2>&1 || {
    tail -n 30 "$log" >&2
    fail "fp_output_stored_thrice_f.cg did not compile"
}

python3 - "$repo_root/tests/shader-compiler" "$log" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
from ucode_decode import groups

gs = groups(sys.argv[2])
if not gs:
    sys.exit("FAIL: no ucode groups decoded from log")

# Find instructions writing to R0.w (mask bit 3 = 0x8) and read their inline constant
# alpha values.
alpha_constants = []
for i, w in enumerate(gs):
    dst = (w[0] >> 1) & 0x3f
    mask = (w[0] >> 9) & 0xf
    none = (w[0] >> 30) & 1
    if not none and dst == 0 and (mask & 0x8):
        if i + 1 < len(gs):
            const_val = gs[i+1][0]
            alpha_constants.append((i, const_val))

expected = [0x3f800000, 0x3f000000, 0x3e800000]
actual_vals = [c[1] for c in alpha_constants]

if actual_vals != expected:
    sys.exit(
        f"FAIL: output store alpha constants out of order: got {[hex(v) for v in actual_vals]}, "
        f"expected {[hex(v) for v in expected]}. "
        f"WAW dependency edges for output destination inverted stores (t_582a7fff regression)"
    )

print(f"scheduler-output-waw-test: ok (alpha stores preserve WAW program order {[hex(v) for v in actual_vals]})")
PY

printf 'PASS: scheduler-output-waw-test\n'
